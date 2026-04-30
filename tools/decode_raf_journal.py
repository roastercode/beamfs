#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
decode_raf_journal.py -- forensic decoder for the beamfs v4 RAF journal

Reads a beamfs v4 image (raw block device dump or .img file), decodes the
on-disk superblock, and dumps every entry of the 64-slot Radiation Event
Journal (RAF) ring buffer in human-readable form.

This is the host-side companion to beamfsd: where beamfsd signs and
propagates events, this tool inspects them post-mortem for audit and
test validation. It mirrors the kernel's beamfs_log_rs_event() write
path and beamfsd's entry_is_valid() validation byte-for-byte.

Author: Aurelien DESBRIERES <aurelien@hackers.camp>
Assisted-by: Claude <claude-opus-4-7>

Usage:
    decode_raf_journal.py <image>
    decode_raf_journal.py <image> --json    (machine-readable output)
    decode_raf_journal.py <image> --raw N   (hex-dump entry N only)

The tool requires only the standard library (struct, json, argparse).
No external dependencies.
"""

import argparse
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

# ====================================================================
# On-disk constants (must match kernel beamfs.h v4 byte-for-byte)
# ====================================================================

BEAMFS_MAGIC                  = 0x4245414D  # "FRTF" little-endian
BEAMFS_VERSION_V1             = 4
BEAMFS_SB_SIZE                = 4096
BEAMFS_RS_JOURNAL_SIZE        = 64
BEAMFS_RS_EVENT_SIZE          = 40

# Offsets in the v4 superblock, computed from struct beamfs_super_block
# layout. See beamfs.h struct beamfs_super_block (v4).
SB_OFF_MAGIC                 = 0
SB_OFF_BLOCK_SIZE            = 4
SB_OFF_BLOCK_COUNT           = 8
SB_OFF_FREE_BLOCKS           = 16
SB_OFF_INODE_COUNT           = 24
SB_OFF_FREE_INODES           = 32
SB_OFF_INODE_TABLE_BLK       = 40
SB_OFF_DATA_START_BLK        = 48
SB_OFF_VERSION               = 56
SB_OFF_FLAGS                 = 60
SB_OFF_CRC32                 = 64
SB_OFF_UUID                  = 68    # 16 bytes
SB_OFF_LABEL                 = 84    # 32 bytes
SB_OFF_RS_JOURNAL            = 116   # 64 * 40 = 2560 bytes
SB_OFF_RS_JOURNAL_HEAD       = 2676  # 1 byte
SB_OFF_BITMAP_BLK            = 2677
SB_OFF_FEAT_COMPAT           = 2685
SB_OFF_FEAT_INCOMPAT         = 2693
SB_OFF_FEAT_RO_COMPAT        = 2701
SB_OFF_DATA_PROTECTION       = 2709
SB_OFF_PAD                   = 2713  # 1383 bytes to 4096

# RS event field offsets within the 40-byte struct
EV_OFF_BLOCK_NO              = 0
EV_OFF_TIMESTAMP             = 8
EV_OFF_SYMBOL_COUNT          = 16
EV_OFF_ENTROPY_Q16_16        = 20
EV_OFF_FLAGS                 = 24
EV_OFF_RESERVED              = 28
EV_OFF_CRC32                 = 32
EV_OFF_PAD                   = 36

# Flags
BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID = 1 << 0

# Sentinel block_no for SB-RS recovery events. Low 13 bits encode the
# subblock index (0..12). Detection is by mask, not equality.
BEAMFS_RS_BLOCK_NO_SB_MARKER  = 0xFFFFFFFFFFFFF000
SB_MARKER_INDEX_MASK         = 0xFFF
SB_MARKER_PREFIX_MASK        = 0xFFFFFFFFFFFFF000

# Q16.16 fractional bits for entropy
BEAMFS_RS_ENTROPY_Q           = 16


# ====================================================================
# CRC32 (mirrors kernel crc32_le with poly 0xEDB88320)
# ====================================================================

def crc32_le(data: bytes) -> int:
    """CRC32 with polynomial 0xEDB88320, initial 0xFFFFFFFF, final XOR.

    Mirrors the kernel beamfs_crc32() and beamfsd crc32_compute() byte
    for byte. Used to validate per-entry re_crc32.
    """
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if (crc & 1) else 0)
    return crc ^ 0xFFFFFFFF


# ====================================================================
# Superblock reader
# ====================================================================

def read_sb(path: Path) -> bytes:
    """Read the first 4096 bytes (superblock) from a raw image file."""
    with open(path, "rb") as f:
        sb = f.read(BEAMFS_SB_SIZE)
    if len(sb) != BEAMFS_SB_SIZE:
        raise ValueError(
            f"short read: got {len(sb)} bytes, expected {BEAMFS_SB_SIZE}")
    return sb


def parse_sb_header(sb: bytes) -> dict:
    """Decode superblock fields up to (but not including) the journal."""
    fields = {
        "magic":              struct.unpack_from("<I", sb, SB_OFF_MAGIC)[0],
        "block_size":         struct.unpack_from("<I", sb, SB_OFF_BLOCK_SIZE)[0],
        "block_count":        struct.unpack_from("<Q", sb, SB_OFF_BLOCK_COUNT)[0],
        "free_blocks":        struct.unpack_from("<Q", sb, SB_OFF_FREE_BLOCKS)[0],
        "inode_count":        struct.unpack_from("<Q", sb, SB_OFF_INODE_COUNT)[0],
        "free_inodes":        struct.unpack_from("<Q", sb, SB_OFF_FREE_INODES)[0],
        "inode_table_blk":    struct.unpack_from("<Q", sb, SB_OFF_INODE_TABLE_BLK)[0],
        "data_start_blk":     struct.unpack_from("<Q", sb, SB_OFF_DATA_START_BLK)[0],
        "version":            struct.unpack_from("<I", sb, SB_OFF_VERSION)[0],
        "flags":              struct.unpack_from("<I", sb, SB_OFF_FLAGS)[0],
        "sb_crc32":           struct.unpack_from("<I", sb, SB_OFF_CRC32)[0],
        "uuid":               sb[SB_OFF_UUID:SB_OFF_UUID + 16].hex(),
        "label":              sb[SB_OFF_LABEL:SB_OFF_LABEL + 32].rstrip(b"\x00").decode("utf-8", errors="replace"),
        "rs_journal_head":    sb[SB_OFF_RS_JOURNAL_HEAD],
        "bitmap_blk":         struct.unpack_from("<Q", sb, SB_OFF_BITMAP_BLK)[0],
        "feat_compat":        struct.unpack_from("<Q", sb, SB_OFF_FEAT_COMPAT)[0],
        "feat_incompat":      struct.unpack_from("<Q", sb, SB_OFF_FEAT_INCOMPAT)[0],
        "feat_ro_compat":     struct.unpack_from("<Q", sb, SB_OFF_FEAT_RO_COMPAT)[0],
        "data_prot_scheme":   struct.unpack_from("<I", sb, SB_OFF_DATA_PROTECTION)[0],
    }
    return fields


def assert_sb_sane(hdr: dict) -> list:
    """Return a list of non-fatal warnings + raise on truly fatal mismatch."""
    warnings = []
    if hdr["magic"] != BEAMFS_MAGIC:
        raise ValueError(
            f"bad magic 0x{hdr['magic']:08x}, expected 0x{BEAMFS_MAGIC:08x}")
    if hdr["version"] != BEAMFS_VERSION_V1:
        raise ValueError(
            f"bad version {hdr['version']}, expected {BEAMFS_VERSION_V1}")
    if hdr["rs_journal_head"] >= BEAMFS_RS_JOURNAL_SIZE:
        warnings.append(
            f"head index {hdr['rs_journal_head']} >= journal size "
            f"{BEAMFS_RS_JOURNAL_SIZE} (will wrap)")
    return warnings


# ====================================================================
# RS journal entry reader
# ====================================================================

def parse_event(raw: bytes, slot: int) -> dict:
    """Decode a single 40-byte beamfs_rs_event entry.

    Returns a dict with all fields plus derived audit info:
      - is_empty: matches mkfs zero-init pattern
      - sentinels_ok: re_reserved == 0 and re_pad == 0
      - crc_ok: CRC32 over [0..32) matches re_crc32
      - is_sb_marker: high 52 bits of block_no == sentinel
      - sb_subblock: extracted index if is_sb_marker
      - timestamp_ns: raw value
      - timestamp_iso: human-readable approximation (assuming epoch base)
    """
    assert len(raw) == BEAMFS_RS_EVENT_SIZE, \
        f"event {slot}: got {len(raw)} bytes, expected {BEAMFS_RS_EVENT_SIZE}"

    block_no    = struct.unpack_from("<Q", raw, EV_OFF_BLOCK_NO)[0]
    timestamp   = struct.unpack_from("<Q", raw, EV_OFF_TIMESTAMP)[0]
    symbols     = struct.unpack_from("<I", raw, EV_OFF_SYMBOL_COUNT)[0]
    entropy_q16 = struct.unpack_from("<I", raw, EV_OFF_ENTROPY_Q16_16)[0]
    flags       = struct.unpack_from("<I", raw, EV_OFF_FLAGS)[0]
    reserved    = struct.unpack_from("<I", raw, EV_OFF_RESERVED)[0]
    crc32_field = struct.unpack_from("<I", raw, EV_OFF_CRC32)[0]
    pad         = struct.unpack_from("<I", raw, EV_OFF_PAD)[0]

    # Empty slot: mirrors beamfsd entry_is_valid() first check
    is_empty = (block_no == 0 and timestamp == 0 and symbols == 0)

    # Sentinels intact: mirrors beamfsd entry_is_valid() second check
    sentinels_ok = (reserved == 0 and pad == 0)

    # CRC: covers bytes [0..32) per format-v4 spec, mirrors kernel
    # super.c offsetof(struct beamfs_rs_event, re_crc32) and beamfsd
    # entry_is_valid() third check.
    crc_computed = crc32_le(raw[:EV_OFF_CRC32])
    crc_ok = (crc_computed == crc32_field)

    # SB recovery sentinel detection by mask
    is_sb_marker = ((block_no & SB_MARKER_PREFIX_MASK)
                    == BEAMFS_RS_BLOCK_NO_SB_MARKER) and not is_empty
    sb_subblock = (block_no & SB_MARKER_INDEX_MASK) if is_sb_marker else None

    # Entropy decode: Q16.16 fixed point, range [0, 3*65536) for B=8 bins
    if flags & BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID:
        entropy_bits = entropy_q16 / (1 << BEAMFS_RS_ENTROPY_Q)
    else:
        entropy_bits = None

    return {
        "slot":            slot,
        "is_empty":        is_empty,
        "block_no":        block_no,
        "block_no_hex":    f"0x{block_no:016x}",
        "is_sb_marker":    is_sb_marker,
        "sb_subblock":     sb_subblock,
        "timestamp_ns":    timestamp,
        "symbols":         symbols,
        "entropy_q16":     entropy_q16,
        "entropy_bits":    entropy_bits,
        "flags":           flags,
        "flags_hex":       f"0x{flags:08x}",
        "entropy_valid":   bool(flags & BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID),
        "reserved":        reserved,
        "pad":             pad,
        "sentinels_ok":    sentinels_ok,
        "crc32_stored":    crc32_field,
        "crc32_stored_hex":f"0x{crc32_field:08x}",
        "crc32_computed":  crc_computed,
        "crc_ok":          crc_ok,
        "raw_hex":         raw.hex(),
    }


def decode_journal(sb: bytes) -> list:
    """Decode all 64 journal slots from the superblock."""
    events = []
    for slot in range(BEAMFS_RS_JOURNAL_SIZE):
        off = SB_OFF_RS_JOURNAL + slot * BEAMFS_RS_EVENT_SIZE
        raw = sb[off:off + BEAMFS_RS_EVENT_SIZE]
        events.append(parse_event(raw, slot))
    return events


# ====================================================================
# Output formatters
# ====================================================================

def fmt_event_text(ev: dict, head: int) -> str:
    """Single-line human-readable event description."""
    head_marker = " <-- HEAD (next write)" if ev["slot"] == head else ""
    if ev["is_empty"]:
        return (f"slot {ev['slot']:2d}: EMPTY"
                f"{head_marker}")

    # Validate event integrity status
    status_parts = []
    if not ev["sentinels_ok"]:
        status_parts.append(f"SENTINEL_FAIL(rsv={ev['reserved']},pad={ev['pad']})")
    if not ev["crc_ok"]:
        status_parts.append(
            f"CRC_FAIL(stored={ev['crc32_stored_hex']},"
            f"computed=0x{ev['crc32_computed']:08x})")
    status = "OK" if not status_parts else " ".join(status_parts)

    # Block number presentation
    if ev["is_sb_marker"]:
        block_str = f"SB_MARKER[subblock={ev['sb_subblock']}]"
    else:
        block_str = f"block={ev['block_no']}"

    # Entropy presentation
    if ev["entropy_valid"]:
        entropy_str = (f" entropy={ev['entropy_bits']:.4f} bits"
                       f" (q16=0x{ev['entropy_q16']:08x})")
    else:
        entropy_str = (f" entropy=invalid"
                       f" (q16=0x{ev['entropy_q16']:08x},"
                       f" flag_bit_clear)")

    return (f"slot {ev['slot']:2d}: {block_str}"
            f" symbols={ev['symbols']}"
            f"{entropy_str}"
            f" flags={ev['flags_hex']}"
            f" ts={ev['timestamp_ns']}ns"
            f" [{status}]"
            f"{head_marker}")


def fmt_summary_text(hdr: dict, events: list, warnings: list) -> str:
    """Human-readable full report."""
    lines = []
    lines.append("=" * 76)
    lines.append("beamfs v4 RAF Journal -- forensic decoder")
    lines.append("=" * 76)
    lines.append("")
    lines.append("Superblock header:")
    lines.append(f"  magic            : 0x{hdr['magic']:08x}"
                 f" ({'FRTF' if hdr['magic'] == BEAMFS_MAGIC else 'BAD'})")
    lines.append(f"  version          : {hdr['version']}"
                 f" ({'v4' if hdr['version'] == BEAMFS_VERSION_V1 else 'NOT v4'})")
    lines.append(f"  block_size       : {hdr['block_size']}")
    lines.append(f"  block_count      : {hdr['block_count']}")
    lines.append(f"  free_blocks      : {hdr['free_blocks']}")
    lines.append(f"  inode_count      : {hdr['inode_count']}")
    lines.append(f"  free_inodes      : {hdr['free_inodes']}")
    lines.append(f"  bitmap_blk       : {hdr['bitmap_blk']}")
    lines.append(f"  data_prot_scheme : {hdr['data_prot_scheme']}")
    lines.append(f"  uuid             : {hdr['uuid']}")
    label = hdr["label"] or "(empty)"
    lines.append(f"  label            : {label}")
    lines.append(f"  rs_journal_head  : {hdr['rs_journal_head']}"
                 " (next slot to write)")
    lines.append(f"  feat_compat      : 0x{hdr['feat_compat']:016x}")
    lines.append(f"  feat_incompat    : 0x{hdr['feat_incompat']:016x}")
    lines.append(f"  feat_ro_compat   : 0x{hdr['feat_ro_compat']:016x}")
    lines.append(f"  sb_crc32         : 0x{hdr['sb_crc32']:08x}")

    if warnings:
        lines.append("")
        lines.append("Warnings:")
        for w in warnings:
            lines.append(f"  - {w}")

    lines.append("")
    lines.append("Journal entries (64 slots):")

    n_used = sum(1 for e in events if not e["is_empty"])
    n_invalid = sum(1 for e in events
                    if not e["is_empty"]
                    and (not e["sentinels_ok"] or not e["crc_ok"]))
    n_sb = sum(1 for e in events if e["is_sb_marker"])

    for ev in events:
        lines.append(f"  {fmt_event_text(ev, hdr['rs_journal_head'])}")

    lines.append("")
    lines.append(f"Stats: {n_used} used / {BEAMFS_RS_JOURNAL_SIZE} total"
                 f" ({n_invalid} integrity failures, {n_sb} SB-marker events)")
    lines.append("")
    return "\n".join(lines)


# ====================================================================
# Main
# ====================================================================

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Forensic decoder for the beamfs v4 RAF journal")
    p.add_argument("image", type=Path,
                   help="path to beamfs v4 raw image (.img file or block dev dump)")
    p.add_argument("--json", action="store_true",
                   help="emit machine-readable JSON instead of text report")
    p.add_argument("--raw", type=int, metavar="N",
                   help="hex-dump entry N (0..63) only, then exit")
    args = p.parse_args(argv)

    try:
        sb = read_sb(args.image)
    except (OSError, ValueError) as e:
        print(f"error reading {args.image}: {e}", file=sys.stderr)
        return 1

    try:
        hdr = parse_sb_header(sb)
        warnings = assert_sb_sane(hdr)
    except ValueError as e:
        print(f"superblock invalid: {e}", file=sys.stderr)
        return 2

    events = decode_journal(sb)

    if args.raw is not None:
        if not (0 <= args.raw < BEAMFS_RS_JOURNAL_SIZE):
            print(f"error: --raw must be in [0,{BEAMFS_RS_JOURNAL_SIZE})",
                  file=sys.stderr)
            return 1
        ev = events[args.raw]
        off = SB_OFF_RS_JOURNAL + args.raw * BEAMFS_RS_EVENT_SIZE
        raw = sb[off:off + BEAMFS_RS_EVENT_SIZE]
        print(f"slot {args.raw} @ disk offset {off} (40 bytes):")
        for i in range(0, BEAMFS_RS_EVENT_SIZE, 16):
            chunk = raw[i:i + 16]
            hexs = " ".join(f"{b:02x}" for b in chunk)
            print(f"  {i:02d}: {hexs}")
        print("")
        print(f"decoded: {fmt_event_text(ev, hdr['rs_journal_head'])}")
        return 0

    if args.json:
        out = {"superblock": hdr, "warnings": warnings, "events": events}
        print(json.dumps(out, indent=2, default=str))
    else:
        print(fmt_summary_text(hdr, events, warnings))

    # Exit code: 0 if all used entries pass integrity, 3 otherwise
    integrity_fails = sum(
        1 for e in events
        if not e["is_empty"]
        and (not e["sentinels_ok"] or not e["crc_ok"]))
    return 3 if integrity_fails else 0


if __name__ == "__main__":
    sys.exit(main())
