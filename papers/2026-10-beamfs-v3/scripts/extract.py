#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""beamfs v3 paper: every number the paper quotes, extracted from the
frozen snapshot in data/raw and cross-checked against an independent
source before it is allowed into a table or a figure.

extract-v3-3: capsule geometry (parity contiguous per codeword, tail
outside the code); hook of each emufi flip recovered (rq/endio, see
DT_PAIR_NS), true block per flip, kernel lines checked against it.

extract-v3-4: xfstests verdicts from the output of check, not from the
harness's summary. BX before 2.4.0 saved as PASS every test xfstests
declined ("[not run]", still counted in "Passed all 1 tests"). The
aarch64 sweep is recounted from the check output BX kept per test; the
x86-64 sweep is the BX 2.5.0 record of its own. Declined tests are
classified by the reason check gave (NOTRUN_RULES).

Standard library only. Writes data/derived/*.csv. Exit status 1 as soon
as one cross-check fails: the paper is never built from a failed
extraction.

Sources, in order of authority:
  1. the BB records (multifs all-records, cluster-records), as written
     by the worker on the nodes;
  2. the signed run manifests;
  3. the output of xfstests' check for every test (BX evidence and
     BX 2.5.0 sweep records), then the BX sweep results;
  4. beamfs-bench.db, used only to cross-check fields known to be sound
     (call_delta, flip_delta, cat_rc, intact, bits_diff). Its
     flips_on_target and flip_event come from the cumulative emufi ring
     and are not used.
"""

import base64
import csv
import gzip
import hashlib
import json
import os
import re
import sqlite3
import sys

W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(W, "data", "raw")
RUNS_DIR = os.path.join(RAW, "runs")
OUT = os.path.join(W, "data", "derived")

# label, analyse dir, multifs dir, manifest, beamfs-bench.db run id
RUNS = [
    ("run2", "beamfs-bench-analyse-full-20261005-202322",
     "beamfs-bench-multifs-20261005-202440",
     "manifest-20261005T191045Z.json", 68),
    ("run3", "beamfs-bench-analyse-full-20261008-075227",
     "beamfs-bench-multifs-20261008-075335",
     "manifest-20261008T064257Z.json", 69),
    ("run4", "beamfs-bench-analyse-full-20261008-092758",
     "beamfs-bench-multifs-20261008-092907",
     "manifest-20261008T081801Z.json", 70),
]
PROBS = (1000, 100000, 1000000)
NODES = ("beamfs-master", "beamfs-compute01", "beamfs-compute02",
         "beamfs-compute03")
SWEEPS = {
    # x86_64_full: BX 2.3.60, superseded (its check outputs were overwritten
    # by the aarch64 sweep); kept in the snapshot, not counted.
    "x86_64_full": "sweep-1790972412.results",
    "aarch64_full": "sweep-1791190821.results",
    "x86_64_476": "sweep-1791196367.results",
    "aarch64_476": "sweep-1791214142.results",
}
# The check output BX 2.3.60 kept per test, copied from its evidence tree
# (trial 001 of each test is the sweep's).
XF_EVIDENCE = os.path.join(RAW, "xfstests-evidence", "live")
# BX 2.5.0 sweep records (meta.txt, verdicts.txt, <test>.check.out).
XF_RECORDS = os.path.join(RAW, "xfstests")
XF_X86_TAG = os.environ.get("XF_X86_TAG", "")
# The x86-64 sweep under BX 2.5.0 that we stopped after the default budget
# of 1900 s killed generic/476: superseded, read for the text.
XF_SUPERSEDED = os.path.join(RAW, "xfstests-superseded")
# What a kernel log must not hold. sysrq w, which BX sends after a kill
# by the budget, prints task states and call traces: those are not here.
KERNEL_BAD = ("WARNING:", "BUG:", "Oops", "LOST POINTER", "Kernel panic",
              "general protection fault", "UBSAN:", "KASAN:", "kernel BUG")
KERNEL_HUNG = "blocked for more than"


def killed_by_budget(reason):
    """BX 2.5.0 writes a kill by the budget as FAIL with the shell's
    report, "Killed ... sudo timeout -k 5 <budget - 30> ./check <test>";
    2.3.60 said it in words."""
    return ("Killed" in reason and "timeout -k" in reason) or "budget" in reason \
        or "did not finish within" in reason
SEAL_X86_IMAGE = "af9d860bbc94127cd842c2a1bd265265c160e3628a1882ea105b20fb6210e08e"
COMMIT_BEAMFS = "1bf151d9f530b44d20837db62ccdeaa7a6c7c785"
FEAT_XFSTESTS = "feat=0x0000000000000000/0x000000000007a000/0x0000000000000000"

# Why xfstests declined a test, by the reason check printed. First match
# wins. Categories: "feature", something beamfs does not implement;
# "environment", something the test image or the virtual machine lacks
# (a device-mapper target, a tool, a log device, space); "scope", a test
# restricted to other filesystems. A reason no rule matches fails the
# extraction, so that every declined test is accounted for.
NOTRUN_RULES = [
    (r"Reflink not supported", "reflink", "feature"),
    (r"Dedupe not supported", "deduplication", "feature"),
    (r"O_DIRECT is not supported", "O_DIRECT", "feature"),
    (r"xfs_io (falloc|fpunch|fzero|fcollapse|finsert)", "fallocate modes", "feature"),
    (r"disk quotas not supported", "quotas", "feature"),
    (r"does not support shutdown", "shutdown ioctl", "feature"),
    (r"No encryption support", "encryption", "feature"),
    (r"attr namespace .* not supported", "extended attributes", "feature"),
    (r"ACLs not supported", "ACLs", "feature"),
    (r"swapfiles are not supported", "swap files", "feature"),
    (r"exchrange|startupdate", "exchange-range", "feature"),
    (r"does not support mknod/mkfifo", "device nodes, FIFOs", "feature"),
    (r"vfstest not support", "idmapped mounts", "feature"),
    (r"does not support freezing", "freeze", "feature"),
    (r"O_TMPFILE is not supported", "O_TMPFILE", "feature"),
    (r"renameat2", "renameat2 flags", "feature"),
    (r"chattr|lsattr", "inode flags", "feature"),
    (r"FITRIM not supported", "FITRIM", "feature"),
    (r"does not support NFS export", "NFS export", "feature"),
    (r"defragmentation not supported", "defragmentation", "feature"),
    (r"does not support metadata journaling", "metadata journaling", "feature"),
    (r"FIBMAP not supported", "FIBMAP", "feature"),
    (r"huge file size", "large files", "feature"),
    (r"creation time not supported", "creation time", "feature"),
    (r"casefold", "case folding", "feature"),
    (r"cross-device copy_file_range", "cross-device copy_file_range", "feature"),
    (r"delayed allocation", "delayed allocation", "feature"),
    (r"xfs_io label", "filesystem label", "feature"),
    (r"fcntl setlease", "file leases", "feature"),
    (r"timestamp limits", "timestamp range warning", "feature"),
    (r"richacl", "richacl", "environment"),
    (r"dm (flakey|thin-pool|snapshot) support|dm_target|dm target", "device-mapper target", "environment"),
    (r"scsi_debug", "scsi_debug", "environment"),
    (r"LOGWRITES_DEV|SCRATCH_LOGDEV", "log device", "environment"),
    (r"utility required|not built", "tool absent from the image", "environment"),
    (r"too small|GB free", "device size", "environment"),
    (r"dax", "DAX", "environment"),
    (r"cgroup2", "cgroup2", "environment"),
    (r"IO_URING", "io_uring", "environment"),
    (r"group not defined", "test user or group", "environment"),
    (r"uid_map", "user namespaces", "environment"),
    (r"not suitable for this filesystem type", "other filesystems only", "scope"),
]

# beamfs data block, capsule layout (INCOMPAT_RS_INTERLEAVE, the mkfs
# default): beamfs_format.h "grouped layout" and edac.c rs_woven_off.
#   data symbol i of codeword k   byte i*16 + k, i < 239 (bytes 0..3823,
#                                 descriptor 3808..3823 included)
#   parity symbols of codeword k  contiguous, bytes 3824 + 16k .. + 15
#   4080 .. 4095                  generation and free, outside the code
# extract-v3-1 applied i*16 + k to the parity too, and put a csum and a
# selfid at 4080 and 4088: both wrong for this layout.
BLOCK = 4096
CODEWORDS = 16
RS_DATA = 239             # data symbols per codeword
RS_BUDGET = 8             # t = (255 - 239) / 2
CODED_END = 3824          # interleaved data symbols end here
PARITY_END = 4080         # parity of the 16 codewords ends here
DIRECT_BLOCKS = 12        # BEAMFS_DIRECT_BLOCKS


def capsule_geom(off):
    """(region, codeword, symbol) of byte @off of a capsule."""
    if off < CODED_END:
        return "coded", off % CODEWORDS, off // CODEWORDS
    if off < PARITY_END:
        p = off - CODED_END
        return "parity", p // 16, RS_DATA + p % 16
    return "tail", None, None


def alternating_cw(off):
    """Codeword of byte @off without RS_INTERLEAVE: s owns [255s, 255s+255)."""
    return off // 255 if off < PARITY_END else None

FAIL = []


def check(cond, what):
    print(("  ok     " if cond else "  ECHEC  ") + what)
    if not cond:
        FAIL.append(what)


def fields(line):
    """KEY=VALUE pairs of one record line; the first occurrence wins."""
    out = {}
    for part in line.rstrip("\n").split("|"):
        if "=" in part:
            key, val = part.split("=", 1)
            out.setdefault(key, val)
    return out


def as_int(val, default=None):
    try:
        return int(val)
    except (TypeError, ValueError):
        return default


def outcome(rec):
    """Outcome of one attack, decided from the record alone."""
    if not rec:
        return "not_exercised"
    calls = as_int(rec.get("CALL_DELTA"), 0)
    flips = as_int(rec.get("FLIP_DELTA"), 0)
    if rec.get("ATTACK") == "SKIP" or (calls == 0 and flips == 0):
        return "not_exercised"
    if rec.get("CAT_RC") != "0":
        return "read_refused"
    if rec.get("HASH_POST") == rec.get("HASH_PRE"):
        return "no_flip" if flips == 0 else "intact"
    return "silent_corruption"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_csv(name, rows, cols):
    path = os.path.join(OUT, name)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print("  ecrit  %s (%d lignes)" % (os.path.relpath(path, W), len(rows)))


def decode_b64gz(val):
    if not val or val == "na":
        return None
    return gzip.decompress(base64.b64decode(val)).decode("utf-8", "replace")


# ---------------------------------------------------------------- multifs

def parse_multifs(path):
    """(fs, prob, attack fields or None, verify fields), in run order.

    The worker writes one ATTACK line then one VERIFY line per
    (filesystem, probability). An attack that printed nothing (squashfs,
    no target) leaves an empty ATTACK line; its filesystem is known only
    from the VERIFY line that follows.
    """
    rows = []
    pending = None
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("ATTACK|"):
                pending = fields(line) if "FS=" in line else {}
            elif line.startswith("VERIFY|"):
                v = fields(line)
                fs = v.get("fs")
                prob = as_int(v.get("prob"))
                a = pending if pending else None
                if a is not None:
                    if a.get("FS") != fs or as_int(a.get("PROB")) != prob:
                        raise SystemExit("ATTACK/VERIFY desalignes dans %s : %s/%s contre %s/%s"
                                         % (path, a.get("FS"), a.get("PROB"), fs, prob))
                rows.append((fs, prob, a, v))
                pending = None
    return rows


MULTIFS_COLS = ["run", "fs", "prob_ppm", "attack_record", "call_delta",
                "flip_delta", "cat_rc", "hash_match", "outcome",
                "rs_corrected_lines", "dmesg_uncorrectable", "dmesg_eio",
                "bits_diff", "n_ranges", "ranges_bytes", "target_ranges",
                "remount_verdict", "remount_files_changed",
                "flip_log_entries", "flip_log_sha256"]


def multifs_rows(label, parsed):
    rows = []
    for fs, prob, a, v in parsed:
        g = (a or {}).get
        ranges = g("TARGET_RANGES", "") or ""
        log = decode_b64gz(g("FLIP_LOG_B64"))
        entries = None
        if log is not None:
            entries = sum(1 for ln in log.splitlines()[1:]
                          if ln and ln.split(",")[1:2] != ["0"])
        rows.append({
            "run": label, "fs": fs, "prob_ppm": prob,
            "attack_record": 1 if a else 0,
            "call_delta": g("CALL_DELTA", ""),
            "flip_delta": g("FLIP_DELTA", ""),
            "cat_rc": g("CAT_RC", ""),
            "hash_match": "" if not a else (1 if g("HASH_POST") == g("HASH_PRE") else 0),
            "outcome": outcome(a),
            "rs_corrected_lines": g("RS_CORRECTED", ""),
            "dmesg_uncorrectable": g("DMESG_UNCORRECTABLE", ""),
            "dmesg_eio": g("DMESG_EIO", ""),
            "bits_diff": g("BITS_DIFF", ""),
            "n_ranges": len(ranges.split(",")) if ranges else 0,
            "ranges_bytes": len(ranges),
            "target_ranges": ranges,
            "remount_verdict": v.get("VERDICT", ""),
            "remount_files_changed": v.get("N_FILES_CHANGED", ""),
            "flip_log_entries": "" if entries is None else entries,
            "flip_log_sha256": g("FLIP_LOG_SHA256", ""),
        })
    return rows


# ---------------------------------------------------------------- cluster

CLUSTER_COLS = ["run", "node", "prob_ppm", "attack", "call_delta",
                "flip_delta", "cat_rc", "hash_match", "outcome",
                "dmesg_uncorrectable", "dmesg_eio", "bits_diff", "n_ranges",
                "ranges_bytes", "target_ranges", "verify_verdict",
                "verify_files_changed", "bb_verdict", "bb_verdict_detail"]

BB_EXPECTED = {
    "intact": ("RS_RECOVERED",),
    "no_flip": ("RS_PASSTHROUGH",),
    "read_refused": ("RS_FAIL_CLOSED", "RS_FAILED"),
    "not_exercised": ("NOT_EXERCISED",),
    "silent_corruption": ("CORRUPTED_DATA", "CORRUPTED"),
}


def parse_cluster(label, path):
    att, ver, der, det = {}, {}, {}, {}
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("ATTACK|"):
                fl = fields(line)
                att[(fl.get("HOST"), as_int(fl.get("prob")))] = fl
            elif line.startswith("VERIFY|"):
                fl = fields(line)
                ver[(fl.get("HOST"), as_int(fl.get("prob")))] = fl
            elif line.startswith("DERIVED|"):
                fl = fields(line)
                key = (fl.get("host"), as_int(fl.get("prob")))
                if "verdict_detail" in fl:
                    det[key] = fl["verdict_detail"]
                elif "verdict" in fl:
                    der[key] = fl["verdict"]
    rows = []
    for prob in PROBS:
        for node in NODES:
            a = att.get((node, prob))
            v = ver.get((node, prob), {})
            g = (a or {}).get
            ranges = g("TARGET_RANGES", "") or ""
            rows.append({
                "run": label, "node": node, "prob_ppm": prob,
                "attack": "absent" if a is None else g("ATTACK", "run"),
                "call_delta": g("CALL_DELTA", ""),
                "flip_delta": g("FLIP_DELTA", ""),
                "cat_rc": g("CAT_RC", ""),
                "hash_match": "" if not a or a.get("ATTACK") == "SKIP"
                else (1 if g("HASH_POST") == g("HASH_PRE") else 0),
                "outcome": outcome(a),
                "dmesg_uncorrectable": g("DMESG_UNCORRECTABLE", ""),
                "dmesg_eio": g("DMESG_EIO", ""),
                "bits_diff": g("BITS_DIFF", ""),
                "n_ranges": len(ranges.split(",")) if ranges else 0,
                "ranges_bytes": len(ranges),
                "target_ranges": ranges,
                "verify_verdict": v.get("VERDICT", ""),
                "verify_files_changed": v.get("N_FILES_CHANGED", ""),
                "bb_verdict": der.get((node, prob), ""),
                "bb_verdict_detail": det.get((node, prob), ""),
            })
    return rows


# ------------------------------------------------------- RS margin (beamfs)

KLINE = re.compile(r"ino=(\d+) iblock=(\d+) subblock=(\d+): (\d+) symbol\(s\) corrected")
MOUNT = re.compile(r"beamfs: mounted (v\d+) .*feat=(0x[0-9a-f]+)/(0x[0-9a-f]+)/(0x[0-9a-f]+)")

MARGIN_COLS = ["run", "prob_ppm", "iblock", "codeword", "symbols_hit",
               "data_symbols_hit", "parity_symbols_hit"]
MSUM_COLS = ["run", "prob_ppm", "flip_delta", "log_tail_used",
             "target_mode", "hook_pairs", "single_rq", "single_endio",
             "single_ambiguous", "dt_pair_max_ns", "dt_other_min_ns",
             "true_data_coded", "true_data_parity", "true_data_tail",
             "true_indirect", "true_indirect_used_slot", "true_outside",
             "ambiguous", "codewords_hit_resolved",
             "max_symbols_per_codeword_union_resolved",
             "max_symbols_per_codeword_per_bio", "kernel_lines",
             "kernel_max_symbols_per_decode", "kernel_target_ino",
             "kernel_lines_other_ino", "kernel_matched_resolved",
             "kernel_matched_ambiguous", "kernel_unmatched",
             "mount_version", "feat_compat", "feat_incompat",
             "feat_ro_compat"]
FLIP_COLS = ["run", "prob_ppm", "seq", "ktime_ns", "dt_prev_ns", "sector",
             "bio_op", "byte_offset", "bit", "rec_block", "off", "hook",
             "true_block", "kind", "iblock", "codeword", "symbol",
             "indirect_slot", "indirect_slot_used"]
PAIR_COLS = ["run", "prob_ppm", "k", "k_ino", "k_iblock", "k_subblock",
             "k_symbols", "matched_seqs", "how"]
FLIPLOG_HEADERS = set()

# emufi 0.8.1 has two read hooks (emufi_hooks_blk.c). blk_update_request
# runs first, with the bio iterator intact, and logs the bio's start
# sector. bio_endio runs next on the same bio; bio_sector_span() then
# assumes the iterator has been consumed and subtracts the transfer size
# from bi_sector. Where the iterator arrives intact, that names the
# block before the one flipped: the flip lands in the first segment of
# the bio, the log says one bio earlier, and the range filter is applied
# to that earlier span. extract-v3-2 paired the first ten kernel
# corrections of each attack with the first flips: every mismatch was
# the kernel naming the next block, same codeword. A bio read twice by
# both hooks shows as two ring entries, sector s then s - 8, a few
# microseconds apart; that is what recovers the hook of each entry.
DT_PAIR_NS = 200000       # rq then endio on one bio: well under 0.2 ms
BIO_SECTORS = 8           # beamfs reads one 4 KiB block per bio


def ranges_blocks(ranges, accepted):
    """Blocks the filter admits: the list if emufi held it, else the
    interval from the first to the last sector (emufi refuses more than
    8 ranges or 512 bytes and falls back on the interval)."""
    spans = []
    for r in ranges.split(","):
        if ":" in r:
            s, e = r.split(":")
            spans.append((int(s), int(e)))
    if not spans:
        return set()
    if not accepted:
        spans = [(min(s for s, _ in spans), max(e for _, e in spans))]
    out = set()
    for s, e in spans:
        out.update(range(s // 8, (e + 7) // 8))
    return out


def rs_margin(label, parsed, margin_rows, msum_rows, flip_rows, pair_rows):
    """Per beamfs attack: where each flip landed, reconstructed from the
    two read hooks of emufi, and checked against the kernel's own
    account of what it corrected.

    The emufi ring is never cleared; the flips of an attack are the last
    FLIP_DELTA entries, and the ring length must equal the running sum
    of FLIP_DELTA, which is checked.

    emufi flips the data of a read bio as it completes; the medium is
    never written. A decode sees only the flips of the bio that carried
    its block: at most one per hook, so at most two symbols per codeword.
    The kernel line "N symbol(s) corrected" is the per-decode count,
    rate-limited to the first lines of an attack.
    """
    running = 0
    for fs, prob, a, _v in parsed:
        if not a:
            continue
        flips = as_int(a.get("FLIP_DELTA"), 0)
        running += flips
        log = decode_b64gz(a.get("FLIP_LOG_B64"))
        if log is None:
            continue
        lines = log.splitlines()
        if lines:
            FLIPLOG_HEADERS.add(lines[0].strip())
        entries = [ln.split(",") for ln in lines[1:] if ln]
        entries = [e for e in entries if len(e) >= 8 and e[1] != "0"]
        entries.sort(key=lambda e: int(e[0]))
        check(len(entries) == running,
              "%s %s %d : anneau emufi cumulatif, %d entrees = somme des FLIP_DELTA %d"
              % (label, fs, prob, len(entries), running))
        if fs != "beamfs" or flips == 0:
            continue
        tail = entries[len(entries) - flips:]
        phys = [as_int(x) for x in a.get("FRAG_PRE_PHYS", "").split(",") if x]
        iblock_of = {b: i for i, b in enumerate(phys)}
        span = set(range(min(phys), max(phys) + 1)) if phys else set()
        gaps = span - set(phys)
        used_ind_slots = max(0, len(phys) - DIRECT_BLOCKS)
        ranges = a.get("TARGET_RANGES", "") or ""
        nr = len(ranges.split(",")) if ranges else 0
        accepted = nr <= 8 and len(ranges) < 512
        tblocks = ranges_blocks(ranges, accepted)

        recs = []
        for e in tail:
            sector, byte_off = int(e[2]), int(e[4])
            recs.append({"run": label, "prob_ppm": prob, "seq": int(e[0]),
                         "ktime_ns": int(e[1]), "dt_prev_ns": "",
                         "sector": sector, "bio_op": e[3],
                         "byte_offset": byte_off, "bit": e[5],
                         "rec_block": (sector * 512 + byte_off) // BLOCK,
                         "off": (sector * 512 + byte_off) % BLOCK,
                         "hook": "", "true_block": "", "kind": "",
                         "iblock": "", "codeword": "", "symbol": "",
                         "indirect_slot": "", "indirect_slot_used": ""})
        dt_pair, dt_other = [], []
        for i in range(1, len(recs)):
            recs[i]["dt_prev_ns"] = recs[i]["ktime_ns"] - recs[i - 1]["ktime_ns"]
        i = 0
        while i < len(recs):
            r = recs[i]
            nx = recs[i + 1] if i + 1 < len(recs) else None
            if (nx is not None and nx["sector"] == r["sector"] - BIO_SECTORS
                    and 0 <= nx["dt_prev_ns"] <= DT_PAIR_NS):
                r["hook"], nx["hook"] = "rq", "endio"
                dt_pair.append(nx["dt_prev_ns"])
                i += 2
                continue
            if nx is not None:
                dt_other.append(nx["dt_prev_ns"])
            # Alone. At 1000000 ppm both hooks fire whenever the filter
            # lets them, so a lone entry at block b is the rq hook of a
            # bio whose endio span (b - 1) the filter refused, or the
            # endio hook of a bio on b + 1 whose own block it refused.
            # Below that, either hook may have drawn alone: ambiguous.
            b = r["rec_block"]
            if prob == 1000000 and (b - 1) not in tblocks:
                r["hook"] = "rq_alone"
            elif prob == 1000000 and (b + 1) not in tblocks:
                r["hook"] = "endio_alone"
            else:
                r["hook"] = "ambiguous"
            i += 1
        n = {"coded": 0, "parity": 0, "tail": 0, "indirect": 0,
             "outside": 0, "ambiguous": 0}
        ind_used = 0
        hits = {}
        per_bio_max = 0
        for idx, r in enumerate(recs):
            if r["hook"] == "ambiguous":
                n["ambiguous"] += 1
                r["kind"] = "ambiguous"
                continue
            tb = r["rec_block"] + (1 if r["hook"] in ("endio", "endio_alone") else 0)
            r["true_block"] = tb
            if tb in iblock_of:
                region, cw, sym = capsule_geom(r["off"])
                r.update({"kind": region, "iblock": iblock_of[tb],
                          "codeword": "" if cw is None else cw,
                          "symbol": "" if sym is None else sym})
                n[region] += 1
                if cw is not None:
                    hits.setdefault((iblock_of[tb], cw), set()).add(sym)
            elif tb in gaps:
                slot = r["off"] // 8
                r.update({"kind": "indirect", "indirect_slot": slot,
                          "indirect_slot_used": 1 if slot < used_ind_slots else 0})
                n["indirect"] += 1
                ind_used += 1 if slot < used_ind_slots else 0
            else:
                r["kind"] = "outside"
                n["outside"] += 1
        per_bio_max = 1 if recs else 0
        for i in range(len(recs) - 1):
            p, q = recs[i], recs[i + 1]
            if (p["hook"] == "rq" and q["hook"] == "endio"
                    and p["codeword"] != "" and p["codeword"] == q["codeword"]):
                per_bio_max = 2
        flip_rows.extend(recs)
        for (ib, cw), syms in sorted(hits.items()):
            margin_rows.append({
                "run": label, "prob_ppm": prob, "iblock": ib, "codeword": cw,
                "symbols_hit": len(syms),
                "data_symbols_hit": sum(1 for s in syms if s < RS_DATA),
                "parity_symbols_hit": sum(1 for s in syms if s >= RS_DATA),
            })

        dm = decode_b64gz(a.get("DMESG_B64")) or ""
        allk = [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
                for m in KLINE.finditer(dm)]
        inos = {}
        for ino, _ib, _sb, _ns in allk:
            inos[ino] = inos.get(ino, 0) + 1
        target_ino = max(inos, key=inos.get) if inos else None
        kseq = [k for k in allk if k[0] == target_ino]
        other_ino = sum(1 for k in allk if k[0] != target_ino)
        # Each kernel line must be accounted for by as many flips as it
        # reports symbols: a resolved flip on that block and codeword,
        # or an ambiguous one whose block is the logged one or the one
        # before. Greedy, in ring order, each flip used once.
        cand = [r for r in recs if r["kind"] in ("coded", "parity")
                or r["kind"] == "ambiguous"]
        window = cand[:3 * len(kseq) + 6]
        used = set()
        m_res = m_amb = unmatched = 0
        for k, (ino, kib, ksb, kn) in enumerate(kseq):
            got, how = [], []
            for c in window:
                if len(got) >= kn:
                    break
                if c["seq"] in used:
                    continue
                if c["kind"] != "ambiguous":
                    if c["iblock"] == kib and c["codeword"] == ksb:
                        got.append(c["seq"])
                        how.append("resolu")
                else:
                    reg, cw, _s = capsule_geom(c["off"])
                    rb = c["rec_block"]
                    ib_rec = iblock_of.get(rb)
                    ib_next = iblock_of.get(rb + 1)
                    if cw == ksb and kib in (ib_rec, ib_next):
                        got.append(c["seq"])
                        how.append("ambigu")
            used.update(got)
            if len(got) == kn:
                m_res += how.count("resolu")
                m_amb += how.count("ambigu")
            else:
                unmatched += 1
            pair_rows.append({"run": label, "prob_ppm": prob, "k": k,
                              "k_ino": ino, "k_iblock": kib,
                              "k_subblock": ksb, "k_symbols": kn,
                              "matched_seqs": " ".join(str(s) for s in got),
                              "how": "+".join(how) if len(got) == kn else "NON"})
        mm = MOUNT.search(dm)
        kmax = max((k[3] for k in kseq), default=0)
        mx = max((len(s) for s in hits.values()), default=0)
        hk = {h: sum(1 for r in recs if r["hook"] == h)
              for h in ("rq", "endio", "rq_alone", "endio_alone", "ambiguous")}
        msum_rows.append({
            "run": label, "prob_ppm": prob, "flip_delta": flips,
            "log_tail_used": len(tail),
            "target_mode": "list" if accepted else "interval",
            "hook_pairs": hk["rq"], "single_rq": hk["rq_alone"],
            "single_endio": hk["endio_alone"],
            "single_ambiguous": hk["ambiguous"],
            "dt_pair_max_ns": max(dt_pair) if dt_pair else "",
            "dt_other_min_ns": min(dt_other) if dt_other else "",
            "true_data_coded": n["coded"], "true_data_parity": n["parity"],
            "true_data_tail": n["tail"], "true_indirect": n["indirect"],
            "true_indirect_used_slot": ind_used,
            "true_outside": n["outside"], "ambiguous": n["ambiguous"],
            "codewords_hit_resolved": len(hits),
            "max_symbols_per_codeword_union_resolved": mx,
            "max_symbols_per_codeword_per_bio": per_bio_max,
            "kernel_lines": len(kseq),
            "kernel_max_symbols_per_decode": kmax,
            "kernel_target_ino": "" if target_ino is None else target_ino,
            "kernel_lines_other_ino": other_ino,
            "kernel_matched_resolved": m_res,
            "kernel_matched_ambiguous": m_amb,
            "kernel_unmatched": unmatched,
            "mount_version": mm.group(1) if mm else "",
            "feat_compat": mm.group(2) if mm else "",
            "feat_incompat": mm.group(3) if mm else "",
            "feat_ro_compat": mm.group(4) if mm else "",
        })
        print("  info   %s beamfs %d : cible %s, paires de crochets %d (dt max %s ns), seuls rq %d endio %d ambigus %d (dt min hors paire %s ns)"
              % (label, prob, "liste" if accepted else "intervalle", hk["rq"],
                 max(dt_pair) if dt_pair else "-", hk["rq_alone"],
                 hk["endio_alone"], hk["ambiguous"],
                 min(dt_other) if dt_other else "-"))
        print("  info   %s beamfs %d : flips resolus donnees %d (parite %d, hors code %d), bloc indirect %d (pointeur en usage %d), hors fichier %d"
              % (label, prob, n["coded"] + n["parity"] + n["tail"], n["parity"],
                 n["tail"], n["indirect"], ind_used, n["outside"]))
        for r in recs:
            if r["kind"] in ("indirect", "outside"):
                print("  info   %s beamfs %d : seq %d secteur %d -> bloc %s %s octet %d bit %s%s"
                      % (label, prob, r["seq"], r["sector"], r["true_block"],
                         r["kind"], r["off"], r["bit"],
                         " slot %s en usage %s" % (r["indirect_slot"], r["indirect_slot_used"])
                         if r["kind"] == "indirect" else ""))
        if kseq:
            check(unmatched == 0,
                  "%s beamfs %d : %d lignes noyau, toutes expliquees par les flips (%d resolus, %d ambigus)"
                  % (label, prob, len(kseq), m_res, m_amb))
        if prob == 1000000:
            check(hk["ambiguous"] == 0,
                  "%s beamfs 1000000 : chaque flip rattache a un crochet (paire ou seul)" % label)
            check(not dt_pair or not dt_other or max(dt_pair) < min(dt_other),
                  "%s beamfs 1000000 : paires de crochets separees des bios suivants (dt max paire %s < dt min hors paire %s)"
                  % (label, max(dt_pair) if dt_pair else "-", min(dt_other) if dt_other else "-"))
        check(kmax <= 2,
              "%s beamfs %d : par decodage, au plus %d symboles corriges selon le noyau (2 crochets x 1 bit)"
              % (label, prob, kmax))
        check(mx <= RS_BUDGET,
              "%s beamfs %d : union sur l'attaque des flips resolus, au plus %d symboles par mot de code (budget %d)"
              % (label, prob, mx, RS_BUDGET))


# ---------------------------------------------------------------- xfstests

def parse_sweep(path):
    rows = {}
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split(None, 4)
            if len(p) < 4:
                continue
            rows[p[0]] = {"verdict": p[1], "seconds": as_int(p[2]),
                          "node": p[3],
                          "reason": p[4].strip() if len(p) > 4 else ""}
    return rows


def parse_check(text):
    """Verdict of one test from the output of xfstests' check.

    NOTRUN when every test check tried was declined ("Ran:" and "Not
    run:" name the same tests), FAIL when check lists failures, PASS
    when it says it passed them all, UNKNOWN otherwise."""
    def names(prefix):
        for ln in text.splitlines():
            if ln.startswith(prefix):
                return sorted(ln[len(prefix):].split())
        return []
    ran, declined, failed = names("Ran:"), names("Not run:"), names("Failures:")
    reason = ""
    for ln in text.splitlines():
        if "[not run]" in ln:
            reason = ln.split("[not run]", 1)[1].strip()
            break
    if declined and declined == ran:
        return "NOTRUN", reason
    if failed:
        return "FAIL", ""
    if any(ln.startswith("Passed all ") and " 0 " not in ln for ln in text.splitlines()):
        return "PASS", ""
    return "UNKNOWN", ""


def platform(text):
    m = re.search(r"^PLATFORM\s+--\s+Linux/(\S+)", text, re.M)
    return m.group(1) if m else ""


def classify(reason):
    for rx, group, cat in NOTRUN_RULES:
        if re.search(rx, reason):
            return group, cat
    return "unclassified", "unclassified"


def parse_meta(path):
    """The start and end sections of a BX 2.5.0 meta.txt, as key=value."""
    out, cur = {}, None
    with open(path) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            m = re.match(r"^=== (start|end) ===$", ln)
            if m:
                cur = out.setdefault(m.group(1), {})
                continue
            if cur is not None and "=" in ln and not ln.startswith(" "):
                k, v = ln.split("=", 1)
                cur.setdefault(k, v)
                if k == "tools_check":
                    out.setdefault("start", {}).setdefault("tools_check", v)
    return out


def identity(facts, manifests):
    """What was measured, by SHA-256: data/raw/identity/identity.txt,
    written on the station from the seals, the deploy directories, the
    kernel configurations and the compiled source trees."""
    path = os.path.join(RAW, "identity", "identity.txt")
    check(os.path.isfile(path), "identite : data/raw/identity/identity.txt present")
    if not os.path.isfile(path):
        return
    kv, trees = {}, []
    with open(path) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if ln.startswith("tree="):
                p = ln[5:].split()
                trees.append((p[0], int(p[1]), int(p[2]), int(p[3])))
            elif "=" in ln:
                k, v = ln.split("=", 1)
                kv[k] = v
    check(kv.get("image_x86") == kv.get("seal_x86_image") == SEAL_X86_IMAGE,
          "identite : image x86-64 = sceau = af9d860b")
    canon = set(m.get("canonical_beamfs_sha256", "") for m in manifests)
    check(len(canon) == 1 and kv.get("image_aarch64") == kv.get("seal_aarch64_image") in canon,
          "identite : image aarch64 = sceau = image des trois manifestes")
    check("CONFIG_BEAMFS_FS=y" in kv.get("config_x86", "") and "CONFIG_BEAMFS_FS=y" in kv.get("config_aarch64", ""),
          "identite : beamfs compile dans le noyau sur les deux architectures")
    for key, what in (("src_commit", "commit 1bf151d"), ("src_layer", "couche 9d43172")):
        p = kv.get(key, "").split()
        check(len(p) == 3 and p[0] == "22" and p[1] == "0" and p[2] == "0",
              "identite : 22 sources du module identiques au manifeste dans le %s (%s)" % (what, kv.get(key)))
    for lab, ident, diff, absent in trees:
        check(diff == 0 and ident >= 21,
              "identite : sources compilees %s identiques au manifeste (%d, %d differents, %d absents)"
              % (lab, ident, diff, absent))
    check(len(trees) >= 4, "identite : au moins 4 arbres compiles compares (%d)" % len(trees))
    for k, v in kv.items():
        facts.append({"run": "identity", "key": k, "value": v, "source": "identity/identity.txt"})
    facts.append({"run": "identity", "key": "trees", "value": str(len(trees)), "source": "identity/identity.txt"})


def xfs476_images(facts):
    """What the harness read in the aarch64 volumes it froze when the
    budget stopped generic/476 (2.3.60, kept in beamfs-keep and copied
    to xfstests-476/aarch64-sweep)."""
    a = []
    for root, _dirs, files in os.walk(os.path.join(RAW, "xfstests-476", "aarch64-sweep")):
        for n in sorted(files):
            p = os.path.join(root, n)
            with open(p, errors="replace") as fh:
                a.append((os.path.relpath(p, RAW), fh.read()))
    a_oor = [n for n, t in a if "outside the device" in t]
    check(a and a_oor, "aarch64 : lecture des images figees de 476, pointeurs hors du peripherique (%s)" % a_oor)
    facts.append({"run": "xfs_aarch64", "key": "frozen_476_oor_files", "value": " ".join(a_oor),
                  "source": "xfstests-476/aarch64-sweep"})


def xfstests(facts):
    """Both sweeps, test by test, from the output of check."""
    res_a = parse_sweep(os.path.join(RAW, "bx", SWEEPS["aarch64_full"]))
    check(len(res_a) == 734, "aarch64 : 734 tests dans les resultats BX (%d)" % len(res_a))
    bad = sorted(t for t, r in res_a.items() if not r["verdict"] == "PASS")
    check(bad == ["generic/476"], "aarch64 : resultats BX, seul generic/476 hors PASS (%s)" % bad)
    r476 = res_a.get("generic/476", {})
    # 2.3.60 left the reason of 476 empty. What it kept of the test, copied
    # to xfstests-476/aarch64-sweep before 476 was run alone: check's
    # output without a verdict, and stall.txt, which the harness writes
    # after a kill; the seconds sit at the budget of 14400 s.
    kept = os.path.join(RAW, "xfstests-476", "aarch64-sweep", "generic-476-001")
    kco = os.path.join(kept, "check.out")
    ktext = open(kco, errors="replace").read() if os.path.isfile(kco) else ""
    kstall = os.path.isfile(os.path.join(kept, "stall.txt"))
    at_budget = (platform(ktext) == "aarch64" and parse_check(ktext)[0] == "UNKNOWN"
                 and kstall and 14370 <= (r476.get("seconds") or 0) <= 14460)
    check(killed_by_budget(r476.get("reason", "")) or at_budget,
          "aarch64 : generic/476 arrete par le budget de temps dans le sweep (%s, %s s, raison '%s', "
          "sortie de check gardee %s, stall.txt %s)"
          % (r476.get("verdict"), r476.get("seconds"), r476.get("reason"),
             parse_check(ktext)[0], kstall))
    facts.append({"run": "xfs_aarch64", "key": "sweep_476", "value": "%s %s"
                  % (r476.get("verdict", ""), r476.get("reason", "")), "source": SWEEPS["aarch64_full"]})

    ev = {}
    for d in sorted(os.listdir(XF_EVIDENCE)):
        m = re.match(r"^(generic|shared)-(\d+)-001$", d)
        p = os.path.join(XF_EVIDENCE, d, "check.out")
        if m is None or not os.path.isfile(p):
            continue
        t = "%s/%s" % (m.group(1), m.group(2))
        text = open(p, errors="replace").read()
        # generic-476-001 is the run of 476 alone (2026-10-05), not the
        # sweep's: the sweep kept no check output for 476. main() reads it.
        if t in res_a and not t == "generic/476" and platform(text) == "aarch64":
            ev[t] = parse_check(text)
    check(set(ev) == set(res_a) - {"generic/476"},
          "aarch64 : sortie de check conservee pour %d tests sur 734, tous sauf generic/476" % len(ev))
    check(all(v[0] in ("PASS", "NOTRUN") for v in ev.values()),
          "aarch64 : aucune sortie de check en echec ni illisible (476 mis a part)")

    tag = XF_X86_TAG
    if not tag:
        recs = sorted(d for d in os.listdir(XF_RECORDS) if d.startswith("sweep-"))
        check(len(recs) == 1, "x86-64 : un seul releve de sweep BX 2.5.0 dans data/raw/xfstests (%s)" % recs)
        tag = recs[-1] if recs else ""
    rec = os.path.join(XF_RECORDS, tag)
    meta = parse_meta(os.path.join(rec, "meta.txt"))
    st, en = meta.get("start", {}), meta.get("end", {})
    check(st.get("bx_version") == "2.5.1" and en.get("bx_version") == "2.5.1",
          "x86-64 : BX 2.5.1 au debut et a la fin (%s, %s)" % (st.get("bx_version"), en.get("bx_version")))
    check(st.get("budget") == "14400" and en.get("budget") == "14400",
          "x86-64 : budget de 14400 s par test (%s, %s)" % (st.get("budget"), en.get("budget")))
    check(st.get("beamfs_commit") == COMMIT_BEAMFS[:7], "x86-64 : beamfs %s" % st.get("beamfs_commit"))
    seal = open(os.path.join(rec, "meta.txt")).read()
    check(SEAL_X86_IMAGE in seal and COMMIT_BEAMFS in seal,
          "x86-64 : sceau de l'image af9d860b et du commit 1bf151d dans le releve")
    check(FEAT_XFSTESTS in st.get("last_mount", ""),
          "x86-64 : volumes formates feat 0x7a000 (%s)" % st.get("last_mount", ""))
    check(st.get("tools_check", "").startswith("fsck.beamfs compared with"),
          "x86-64 : fsck.beamfs du noeud compare a celui du depot")
    check(st.get("tests") == "734", "x86-64 : 734 tests a lancer (%s)" % st.get("tests"))
    for k in ("kernel_image", "uname", "tainted", "beamfs_loaded", "tool", "last_mount",
              "at", "uptime", "budget"):
        facts.append({"run": "xfs_x86", "key": "start_" + k, "value": st.get(k, ""), "source": tag + "/meta.txt"})
    for k in ("tainted", "at", "uptime", "budget", "recoveries"):
        facts.append({"run": "xfs_x86", "key": "end_" + k, "value": en.get(k, ""), "source": tag + "/meta.txt"})
    facts.append({"run": "xfs_x86", "key": "tag", "value": tag, "source": "data/raw/xfstests"})
    for name in ("meta.txt", "verdicts.txt"):
        facts.append({"run": "xfs_x86", "key": name.split(".")[0] + "_sha256",
                      "value": sha256_file(os.path.join(rec, name)), "source": tag + "/" + name})
    facts.append({"run": "xfs_aarch64", "key": "check_outputs", "value": str(len(ev)),
                  "source": "xfstests-evidence/live"})
    tools = [ln[5:] for ln in seal.splitlines() if ln.startswith("tool=")]
    for t in tools[:2]:
        name = t.split()[0]
        facts.append({"run": "xfs_x86", "key": "tool_" + name, "value": t, "source": tag + "/meta.txt"})

    ver = {}
    with open(os.path.join(rec, "verdicts.txt")) as f:
        for ln in f:
            p = ln.rstrip("\n").split(None, 3)
            if len(p) >= 3:
                ver[p[0]] = {"verdict": p[1], "seconds": as_int(p[2]),
                             "reason": p[3].strip() if len(p) > 3 else ""}
    check(set(ver) == set(res_a), "x86-64 : les memes 734 tests que l'aarch64 (%d)" % len(ver))
    agree, missing = 0, []
    for t, v in ver.items():
        p = os.path.join(rec, t.replace("/", "-") + ".check.out")
        if not os.path.isfile(p):
            missing.append((t, v["verdict"]))
            continue
        got = parse_check(open(p, errors="replace").read())[0]
        budget_kill = v["verdict"] == "FAIL" and killed_by_budget(v["reason"])
        if got == v["verdict"] or (v["verdict"] in ("NOVERDICT", "HANG") and got == "UNKNOWN") \
                or (budget_kill and got == "UNKNOWN"):
            agree += 1
        else:
            check(False, "x86-64 : %s verdict BX %s, sortie de check %s" % (t, v["verdict"], got))
    killed = sorted(t for t, v in ver.items() if v["verdict"] == "FAIL" and killed_by_budget(v["reason"]))
    print("  info   x86-64 : arretes par le budget : %s" % killed)
    for t in killed:
        facts.append({"run": "xfs_x86", "key": "budget_" + t, "value": "%s %s"
                      % (ver[t]["seconds"], ver[t]["reason"]), "source": tag + "/verdicts.txt"})
    if "generic/476" in ver:
        facts.append({"run": "xfs_x86", "key": "sweep_476_seconds",
                      "value": str(ver["generic/476"]["seconds"]), "source": tag + "/verdicts.txt"})
        facts.append({"run": "xfs_x86", "key": "sweep_476_verdict", "value": "%s %s"
                      % (ver["generic/476"]["verdict"], "budget" if killed_by_budget(ver["generic/476"]["reason"]) else ""),
                      "source": tag + "/verdicts.txt"})

    # The kernel log of each test, from the trace in the record: BX clears
    # dmesg before a test and keeps it after, and the trace carries every
    # case file under "===== <case>/<file> (<n> bytes) =====".
    tr = [os.path.join(rec, n) for n in sorted(os.listdir(rec)) if n.endswith(".trace")]
    check(len(tr) == 1, "x86-64 : une trace dans le releve (%s)" % tr)
    logs, bad, hung, tree_clean = {}, [], [], 0
    if tr:
        cur = None
        with open(tr[0], errors="replace") as fh:
            for ln in fh:
                if ln.startswith("===== ") and ln.rstrip().endswith(" ====="):
                    m = re.match(r"^===== ((?:generic|shared)-\d+)-001/dmesg \(", ln)
                    cur = m.group(1) if m else None
                    if cur is not None:
                        logs[cur] = []
                    continue
                if cur is not None:
                    logs[cur].append(ln.rstrip("\n"))
    for case, lines in sorted(logs.items()):
        text = "\n".join(lines)
        b = [x for x in lines if any(k in x for k in KERNEL_BAD)
             or ("treecheck:" in x and "no lost pointer seen" not in x)]
        if b:
            bad.append((case, b[:3]))
        if KERNEL_HUNG in text:
            hung.append(case)
        if "treecheck: no lost pointer seen" in text:
            tree_clean += 1
    want = set(t.replace("/", "-") for t in ver)
    check(set(logs) == want, "x86-64 : journal noyau de chaque test dans la trace (%d sur %d)"
          % (len(logs), len(want)))
    check(not bad, "x86-64 : aucun avertissement, oops ni rapport du verificateur d'arbre dans %d journaux (%s)"
          % (len(logs), bad))
    print("  info   x86-64 : %d journaux noyau, verificateur sans pointeur perdu dans %d ; taches bloquees : %s"
          % (len(logs), tree_clean, hung))
    facts.append({"run": "xfs_x86", "key": "kernel_logs", "value": str(len(logs)), "source": tag + "/*.trace"})
    facts.append({"run": "xfs_x86", "key": "kernel_tree_clean", "value": str(tree_clean),
                  "source": tag + "/*.trace"})
    facts.append({"run": "xfs_x86", "key": "kernel_hung", "value": " ".join(hung) or "none",
                  "source": tag + "/*.trace"})

    # The superseded x86-64 sweep under BX 2.5.0, stopped after
    # generic/476 hit the default budget of 1900 s.
    sups = sorted(d for d in os.listdir(XF_SUPERSEDED) if d.startswith("sweep-")) \
        if os.path.isdir(XF_SUPERSEDED) else []
    check(len(sups) == 1, "x86-64 remplace : un releve BX 2.5.0 (%s)" % sups)
    if sups:
        sd = os.path.join(XF_SUPERSEDED, sups[0])
        sm = parse_meta(os.path.join(sd, "meta.txt"))
        stext = open(os.path.join(sd, "meta.txt")).read()
        check(sm.get("start", {}).get("bx_version") == "2.5.0" and SEAL_X86_IMAGE in stext
              and COMMIT_BEAMFS in stext and "budget" not in sm.get("start", {}),
              "x86-64 remplace : BX 2.5.0, sceau af9d860b, commit 1bf151d, budget non enregistre")
        sv = {}
        with open(os.path.join(sd, "verdicts.txt")) as fh:
            for ln in fh:
                p = ln.rstrip("\n").split(None, 3)
                if len(p) >= 3:
                    sv[p[0]] = (p[1], as_int(p[2]), p[3] if len(p) > 3 else "")
        s476 = sv.get("generic/476", ("", 0, ""))
        check(s476[0] == "FAIL" and killed_by_budget(s476[2]),
              "x86-64 remplace : generic/476 tue par le budget (%s, %s s)" % (s476[0], s476[1]))
        for key, val in (("sweep_476_seconds", str(s476[1])), ("done", str(len(sv))),
                         ("tag", sups[0]),
                         ("verdicts_sha256", sha256_file(os.path.join(sd, "verdicts.txt")))):
            facts.append({"run": "xfs_superseded", "key": key, "value": val,
                          "source": sups[0] + "/verdicts.txt"})

    check(all(vv == "NOVERDICT" or (vv == "FAIL" and killed_by_budget(ver[tt]["reason"])) for tt, vv in missing),
          "x86-64 : sortie de check presente pour tout test qui a un verdict (sans : %s)" % missing)
    check(agree == len(ver) - len(missing),
          "x86-64 : verdict BX = sortie de check pour %d tests sur %d" % (agree, len(ver) - len(missing)))

    rows, unc = [], []
    for t in sorted(res_a):
        a = ev.get(t, ("FAIL", "") if t == "generic/476" else ("UNKNOWN", ""))
        x = ver.get(t, {"verdict": "", "seconds": "", "reason": ""})
        xr = x["reason"] if x["verdict"] == "NOTRUN" else ""
        ag, ac = classify(a[1]) if a[0] == "NOTRUN" else ("", "")
        xg, xc = classify(xr) if x["verdict"] == "NOTRUN" else ("", "")
        for g, rr in ((ag, a[1]), (xg, xr)):
            if g == "unclassified":
                unc.append(rr)
        rows.append({"test": t,
                     "x86_64_verdict": x["verdict"], "x86_64_seconds": x["seconds"],
                     "x86_64_reason": xr, "x86_64_group": xg, "x86_64_category": xc,
                     "aarch64_verdict": a[0], "aarch64_seconds": res_a[t]["seconds"],
                     "aarch64_reason": a[1], "aarch64_group": ag, "aarch64_category": ac})
    check(not unc, "raisons de non-execution toutes classees (%d non classees : %s)"
          % (len(unc), sorted(set(unc))))
    return rows


# ---------------------------------------------------------------- main

def main():
    os.makedirs(OUT, exist_ok=True)
    print("=== instantane")
    check(os.path.isfile(os.path.join(RAW, "SHA256SUMS")), "data/raw/SHA256SUMS present")

    all_m, all_c, margin, msum, facts = [], [], [], [], []
    manifests = []
    flip_rows, pair_rows = [], []
    db = sqlite3.connect("file:%s?mode=ro" % os.path.join(RAW, "beamfs-bench.db"), uri=True)

    for label, adir, mdir, manifest, dbrun in RUNS:
        print("=== %s (%s)" % (label, adir))
        a_rec = os.path.join(RUNS_DIR, adir, "multifs-all-records.txt")
        m_rec = os.path.join(RUNS_DIR, mdir, "all-records.txt")
        check(sha256_file(a_rec) == sha256_file(m_rec),
              "%s : multifs-all-records.txt du run = all-records.txt du dossier multifs" % label)
        parsed = parse_multifs(a_rec)
        # btrfs: the record's kernel log slice carries no checksum line; the
        # cause of the refusal is read from the node's own kernel log.
        bt = [a for fs, _p, a, _v in parsed if fs == "btrfs" and a]
        in_rec = sum(1 for a in bt if "csum failed" in (decode_b64gz(a.get("DMESG_B64")) or ""))
        fl = os.path.join(RUNS_DIR, adir, "forensics-beamfs-compute01", "dmesg.log")
        ncs = 0
        if os.path.isfile(fl):
            with open(fl, errors="replace") as fh:
                ncs = sum(1 for ln in fh if "BTRFS warning" in ln and "csum failed" in ln
                          and "mirror 1" in ln)
        check(ncs >= 1 and in_rec == 0,
              "%s : btrfs, %d ligne(s) csum failed dans le journal du noeud, %d dans les %d enregistrements"
              % (label, ncs, in_rec, len(bt)))
        facts.append({"run": label, "key": "btrfs_csum_lines_node", "value": str(ncs),
                      "source": "forensics-beamfs-compute01/dmesg.log"})
        rows = multifs_rows(label, parsed)
        check(len(rows) == 15, "%s : 15 couples (FS, probabilite) en multifs, lus %d" % (label, len(rows)))
        check(sum(1 for r in rows if r["attack_record"]) == 12,
              "%s : 12 attaques enregistrees (squashfs sans enregistrement)" % label)

        # Cross-check against the database, sound fields only.
        dbrows = db.execute(
            "select fs, call_delta, flip_delta, cat_rc, intact, bits_diff "
            "from measurement where run_id = ? order by id", (dbrun,)).fetchall()
        byfs = {}
        for fs, cd, fd, cr, it, bd in dbrows:
            byfs.setdefault(fs, []).append((cd, fd, cr, it, bd))
        for r in rows:
            if not r["attack_record"]:
                continue
            seq = byfs.get(r["fs"], [])
            k = PROBS.index(r["prob_ppm"])
            if k >= len(seq):
                check(False, "%s %s %d : ligne absente de la base" % (label, r["fs"], r["prob_ppm"]))
                continue
            cd, fd, cr, it, bd = seq[k]
            intact = 1 if (r["cat_rc"] == "0" and r["hash_match"] == 1) else 0
            same = (str(cd) == r["call_delta"] and str(fd) == r["flip_delta"]
                    and str(cr) == r["cat_rc"] and it == intact
                    and str(bd) == r["bits_diff"])
            check(same, "%s %s %d : enregistrement = base (appels, flips, cat_rc, intact, bits)"
                  % (label, r["fs"], r["prob_ppm"]))
        all_m.extend(rows)

        c_rows = parse_cluster(label, os.path.join(RUNS_DIR, adir, "cluster-records.txt"))
        for r in c_rows:
            ok = r["bb_verdict"] in BB_EXPECTED.get(r["outcome"], ())
            check(ok, "%s %s %d : issue %s, verdict BB %s"
                  % (label, r["node"], r["prob_ppm"], r["outcome"], r["bb_verdict"]))
        all_c.extend(c_rows)

        rs_margin(label, parsed, margin, msum, flip_rows, pair_rows)

        with open(os.path.join(RUNS_DIR, manifest)) as f:
            man = json.load(f)
        manifests.append(man)
        for key in ("commit_beamfs", "commit_yocto", "commit_bench",
                    "canonical_beamfs_sha256", "chain_verdict", "overall_rc",
                    "started_at"):
            facts.append({"run": label, "key": key, "value": man.get(key, ""),
                          "source": manifest})
        facts.append({"run": label, "key": "manifest_sha256",
                      "value": sha256_file(os.path.join(RUNS_DIR, manifest)),
                      "source": manifest})

    print("=== en-tetes flip_log vus : %d" % len(FLIPLOG_HEADERS))
    for h in sorted(FLIPLOG_HEADERS):
        print("  info   " + h)
    hdr = sorted(FLIPLOG_HEADERS)[0].split(",") if len(FLIPLOG_HEADERS) == 1 else []
    check(len(hdr) >= 8 and "sector" in hdr[2] and ("byte" in hdr[4] or "off" in hdr[4]),
          "flip_log : un seul en-tete, colonne 3 = secteur, colonne 5 = octet")
    print("=== attentes de fond")
    run4c = [r for r in all_c if r["run"] == "run4"]
    check(all(as_int(r["call_delta"], 0) > 0 for r in run4c),
          "run4 : les 12 cases du cluster attaquees")
    run3c01 = [r for r in all_c if r["run"] == "run3" and r["node"] == "beamfs-compute01"]
    check(all(r["outcome"] == "not_exercised" for r in run3c01),
          "run3 : compute01 jamais attaque (liste refusee par emufi)")
    run2c = [r for r in all_c if r["run"] == "run2"]
    check(all(r["outcome"] == "not_exercised" for r in run2c),
          "run2 : cluster jamais attaque (liste btrfs restee en place)")

    print("=== identite de ce qui a ete mesure")
    identity(facts, manifests)

    print("=== xfstests")
    sw = {k: parse_sweep(os.path.join(RAW, "bx", v)) for k, v in SWEEPS.items()}
    check(sw["x86_64_476"].get("generic/476", {}).get("verdict") == "PASS",
          "x86-64 : generic/476 seul PASS (BX 2.3.60, avant le sweep 2.5.0)")
    check(sw["aarch64_476"].get("generic/476", {}).get("verdict") == "PASS",
          "aarch64 : generic/476 seul PASS")
    p476 = os.path.join(XF_EVIDENCE, "generic-476-001", "check.out")
    t476 = open(p476, errors="replace").read() if os.path.isfile(p476) else ""
    check(platform(t476) == "aarch64" and parse_check(t476)[0] == "PASS",
          "aarch64 : sortie de check de generic/476 seul conservee, PASS")
    xrows = xfstests(facts)
    xfs476_images(facts)
    for t, key in (("generic/476", "x86_64_476"), ("generic/476", "aarch64_476")):
        r = sw[key].get(t, {})
        facts.append({"run": "bx", "key": key + "_seconds", "value": r.get("seconds", ""),
                      "source": SWEEPS[key]})
    for key, name in SWEEPS.items():
        facts.append({"run": "bx", "key": key + "_sha256",
                      "value": sha256_file(os.path.join(RAW, "bx", name)), "source": name})

    if FAIL:
        # Nothing is written when a check fails, as the appendix says: the
        # v3-8 run wrote its CSV files and then reported the failure.
        print("=== bilan : %d controle(s) en echec, rien n'est ecrit" % len(FAIL))
        for f in FAIL:
            print("  ECHEC  " + f)
        return 1
    print("=== ecriture")
    write_csv("multifs.csv", all_m, MULTIFS_COLS)
    write_csv("cluster.csv", all_c, CLUSTER_COLS)
    write_csv("rs_margin.csv", margin, MARGIN_COLS)
    write_csv("rs_margin_summary.csv", msum, MSUM_COLS)
    write_csv("flips_beamfs.csv", flip_rows, FLIP_COLS)
    write_csv("rs_kernel_pairs.csv", pair_rows, PAIR_COLS)
    write_csv("xfstests.csv", xrows, ["test", "x86_64_verdict", "x86_64_seconds",
                                      "x86_64_reason", "x86_64_group", "x86_64_category",
                                      "aarch64_verdict", "aarch64_seconds",
                                      "aarch64_reason", "aarch64_group", "aarch64_category"])
    write_csv("facts.csv", facts, ["run", "key", "value", "source"])

    print("=== bilan : %d controle(s) en echec" % len(FAIL))
    for f in FAIL:
        print("  ECHEC  " + f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
