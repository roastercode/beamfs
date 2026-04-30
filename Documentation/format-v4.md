# beamfs On-Disk Format Specification (v4)

**Status:** Authoritative specification for `BEAMFS_VERSION_V1` with
`BEAMFS_FORMAT_V4` superblock layout.
**Audience:** Kernel reviewers, certification auditors, forensic analysts,
userspace tooling authors.
**Companion documents:**
`Documentation/threat-model.md` (failure model and adversarial scope),
`Documentation/design.md` (architectural rationale, partially superseded
by this document for v4 specifics),
`Documentation/known-limitations.md` (explicit non-goals).

This document is the single source of truth for the v4 on-disk format.
All field offsets, sizes, encodings, invariants, and recovery procedures
described here are normative. The kernel implementation, the userspace
`mkfs.beamfs` formatter, and any third-party reader MUST conform.

---

## 1. Scope and conventions

### 1.1 Scope

beamfs v4 is the on-disk format produced by `mkfs.beamfs` and consumed by
the beamfs kernel module starting from kernel module version 0.1.0. It
extends the v3 layout with:

1. A 24 → 40 byte enlargement of `struct beamfs_rs_event`, adding a
   per-event Shannon entropy field, a flags field, and a per-entry CRC32.
2. An 8 → 13 subblock enlargement of the superblock RS protection
   layout, raising correction capacity from 64 to 104 symbol errors per
   superblock.
3. A revised `s_pad[]` layout (1383 bytes vs. 2407 bytes in v3) reflecting
   the 1024-byte enlargement of `s_rs_journal[]`.

Block layout, inode layout, bitmap layout, and the on-disk endpoints of
the data path are unchanged from v3 and are described in
`Documentation/design.md`. Where this document and `design.md` disagree,
**this document is authoritative for v4**.

### 1.2 Conventions

- All multi-byte integer fields are little-endian (`__le16`, `__le32`,
  `__le64`).
- All structures are `__packed`. Implicit padding is forbidden.
- All structure sizes are enforced at compile time via `BUILD_BUG_ON()`.
- All reserved fields MUST be zero on write. Non-zero reserved fields
  are structural sentinels and produce CRC mismatch on read.
- Block size is fixed at 4096 bytes (`BEAMFS_BLOCK_SIZE`).
- Reed-Solomon codewords are RS(255,239) shortened over GF(2^8) with the
  standard `lib/reed_solomon` Linux kernel implementation.
- "Symbol" in this document means an 8-bit Reed-Solomon symbol unless
  otherwise specified.
- Citations to `threat-model.md` use the form "TM §N.N".

---

## 2. Format identification and version policy

### 2.1 Magic and version

BEAMFS_MAGIC            = 0x4245414D   ("BEAM" little-endian)
BEAMFS_VERSION_CURRENT  = BEAMFS_VERSION_V1 = 1


The on-disk `s_magic` and `s_version` fields are the primary identifiers.
`s_magic` distinguishes beamfs from the predecessor FTRFS format. Mount
policy is **strict equality** with `BEAMFS_VERSION_CURRENT`. There are
no v2 or v3 disk images in the wild; the v2/v3 format names refer to
intermediate code-internal layouts that never reached a release.

### 2.2 Format version vs. kernel module version

- **Format version** (`s_version`, this document): the on-disk
  representation. Mount-blocking when changed.
- **Kernel module version** (`MODULE_VERSION` macro): the implementation.
  May change without bumping the format. The format-to-module mapping is
  recorded in the release notes.

A kernel module that does not recognise the format version refuses to
mount the volume. Forward compatibility is reserved for a future v5
format and is not provided by v4.

---

## 3. Block layout

The block layout is unchanged from v3 and reproduced here for
self-containment:

Block 0            superblock (4096 bytes, magic 0x4245414D)
Block 1..N         inode table (16 inodes per block, 256 bytes per inode)
Block N+1          on-disk block bitmap (RS FEC protected)
Block N+2          root directory data
Block N+3..end     data blocks (4096 bytes each)


`N` is determined at format time by `mkfs.beamfs` from the requested
inode count. The default configuration uses `N=4` (64 inodes total).

The block bitmap is mandatory and is itself protected by Reed-Solomon
FEC across 16 subblocks of 239 user data bytes plus 16 parity bytes per
subblock, identical to the data-block layout described in §7.

---

## 4. Superblock (block 0)

### 4.1 Canonical layout

```c
struct beamfs_super_block {
    __le32  s_magic;            /*    0..3   BEAMFS_MAGIC                  */
    __le32  s_block_size;       /*    4..7   always 4096                   */
    __le64  s_block_count;      /*    8..15  total blocks on device        */
    __le64  s_free_blocks;      /*   16..23  free data blocks              */
    __le64  s_inode_count;      /*   24..31  total inodes                  */
    __le64  s_free_inodes;      /*   32..39  free inodes                   */
    __le64  s_inode_table_blk;  /*   40..47  first inode table block       */
    __le64  s_data_start_blk;   /*   48..55  first data block              */
    __le32  s_version;          /*   56..59  BEAMFS_VERSION_CURRENT        */
    __le32  s_flags;            /*   60..63  reserved (zero)               */
    __le32  s_crc32;            /*   64..67  CRC32 over coverage regions   */
    __u8    s_uuid[16];         /*   68..83  volume UUID                   */
    __u8    s_label[32];        /*   84..115 volume label, NUL-padded      */
    struct beamfs_rs_event
            s_rs_journal[64];   /*  116..1651 Electromagnetic Resilience J */
    __u8    s_rs_journal_head;  /* 1652      ring buffer write head (0-63) */
    __le64  s_bitmap_blk;       /* 1653..1660 bitmap block number          */
    __le64  s_feat_compat;      /* 1661..1668 informational features       */
    __le64  s_feat_incompat;    /* 1669..1676 incompatible features        */
    __le64  s_feat_ro_compat;   /* 1677..1684 RO-compat features           */
    __le32  s_data_protection_scheme; /* 1685..1688 enum, see §10          */
    __u8    s_pad[1383];        /* 1689..3071 padding incl. SB RS parity   */
                                /* 3888..4095 SB RS parity zone (208 b)    */
} __packed; /* 4096 bytes total, BUILD_BUG_ON enforced */
```

The `s_pad` array is 1383 bytes long and includes the 208-byte trailing
zone reserved for superblock Reed-Solomon parity. See §5.

### 4.2 Coverage and CRC32

`s_crc32` is computed over **two non-contiguous regions** chained via
`crc32_le()` without intermediate XOR:

- **Region A:** `[0, offsetof(s_crc32))` - 64 bytes (header, counters,
  version, flags). This precedes the checksum.
- **Region B:** `[offsetof(s_uuid), offsetof(s_pad))` - 2645 bytes (UUID,
  label, RS journal, journal head, bitmap block, feature fields,
  protection scheme).

Total coverage: **2709 bytes** (`BEAMFS_SB_RS_COVERAGE_BYTES`). The
padding `s_pad` is excluded.

The userspace `mkfs.beamfs::crc32_sb()` MUST produce a byte-identical
CRC32 to the kernel `beamfs_crc32_sb()` for the same superblock
contents, otherwise the volume will fail to mount.

### 4.3 Feature fields (v3+)

Three 64-bit feature bitmaps are present and consulted at mount time:

- `s_feat_compat` - informational only. Unknown bits are tolerated.
- `s_feat_incompat` - refuse mount entirely if any unknown bit is set.
- `s_feat_ro_compat` - force read-only mount if any unknown bit is set.

In `BEAMFS_VERSION_V1` no feature bits are allocated; all three masks
are zero on a freshly formatted volume. Future feature bits will be
documented in a successor format specification.

---

## 5. Superblock Reed-Solomon protection

### 5.1 Coverage region

The 2709 logical bytes covered by `s_crc32` are also protected by
Reed-Solomon FEC. The CRC and the RS coverage are intentionally
**identical** so that a corruption either flips a CRC mismatch (detected,
recoverable via RS) or is corrected by RS without ever surfacing to the
CRC verifier.

The 2709 covered bytes are serialised into a contiguous **2743-byte
staging buffer** by the kernel mount-time recovery routine. The 34
extra bytes are zero padding required to round up to 13 RS subblocks of
**211 user data bytes** each (`13 × 211 = 2743`).

```
BEAMFS_SB_RS_COVERAGE_BYTES  = 2709   /* logical bytes, CRC range  */
BEAMFS_SB_RS_STAGING_BYTES   = 2743   /* 13 * 211, padded          */
BEAMFS_SB_RS_DATA_LEN        = 211    /* per shortened subblock    */
BEAMFS_SB_RS_SUBBLOCKS       = 13     /* total subblocks           */
BEAMFS_SB_RS_PARITY_BYTES    = 208    /* 13 * 16                   */
```


### 5.2 Parity placement

The 208 bytes of RS parity (13 × 16) are stored at:

disk offset 3888..4095  (== end of superblock - 208)
== s_pad[1175..1382]    (BEAMFS_SB_RS_S_PAD_INDEX = 1175)


This trailing position is **stable against future format evolution**:
new fields go into `s_pad` before the parity zone, and
`BEAMFS_SB_RS_S_PAD_INDEX` is computed from `offsetof(s_pad)` so the
layout updates atomically.

### 5.3 Correction capacity

Each of the 13 subblocks is an independent RS(255,239) shortened
codeword and tolerates up to **8 symbol errors** (`BEAMFS_RS_PARITY/2 =
16/2`). Total correction capacity across the superblock: **104 symbol
errors when distributed across subblocks**, fewer when concentrated in
one subblock.

This is a deliberate design choice for burst tolerance per TM §6.2:
distributing parity across multiple short codewords gives independent
failure-correctable regions, which trade off against per-codeword
correction radius. Thirteen subblocks were chosen to absorb the v3 → v4
journal enlargement while keeping parity in the trailing 208-byte zone.

### 5.4 Growth v3 → v4

| Field                         | v3           | v4           | Δ          |
|-------------------------------|--------------|--------------|------------|
| `BEAMFS_SB_RS_COVERAGE_BYTES` | 1685         | 2709         | +1024      |
| `BEAMFS_SB_RS_STAGING_BYTES`  | 2743         | 2743         | unchanged  |
| `BEAMFS_SB_RS_SUBBLOCKS`      | 8            | 13           | +5         |
| `BEAMFS_SB_RS_DATA_LEN`       | 211          | 211          | unchanged  |
| `BEAMFS_SB_RS_PARITY_BYTES`   | 128          | 208          | +80        |
| `s_pad[]` length              | 2407         | 1383         | -1024      |
| `s_rs_journal[]` entry size   | 24           | 40           | +16        |
| `s_rs_journal[]` total length | 1536 (24×64) | 2560 (40×64) | +1024      |
| Correction capacity           | 64 sym       | 104 sym      | +40 sym    |

The v3 → v4 transition expands CRC/RS coverage by exactly the
`s_rs_journal[]` enlargement (1024 bytes), keeping the staging buffer
size constant at 2743 bytes by repacking into 13 shorter subblocks
instead of 8 longer ones. The 80 additional parity bytes (208 - 128)
come from `s_pad[]`, which shrinks by 1024 bytes total (1024 absorbed
by journal growth, balanced by the geometry change).

---

## 6. Electromagnetic Resilience Journal

### 6.1 Purpose

The Electromagnetic Resilience Journal is a fixed-size, on-disk,
ring-buffer log of Reed-Solomon FEC events that have occurred during
the lifetime of the volume. The on-disk symbol `s_rs_journal[]` is
preserved across versions for source compatibility; the journal's role
broadened from radiation-only event recording (v1–v3 nomenclature) to
the full electromagnetic resilience taxonomy (v4, see TM §2) without
any change to the on-disk byte layout. It serves three audiences:

1. **Operators** - observe the rate and severity of FEC events over time
   to plan media replacement or environmental mitigation.
2. **Forensic analysts** - discriminate Family A (stochastic EM
   perturbations, TM §2.1) from Family B (adversarial EM events,
   TM §2.2), and identify saturation-boundary events (TM §2.3),
   using temporal clustering, spatial clustering across `re_block_no`,
   the per-event Shannon entropy estimate, and the
   `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE` flag.
3. **Certification auditors** - document that the filesystem maintains
   a tamper-evident record of every recovery action it has performed,
   per TM §6.4.

### 6.2 Sizing

The journal contains exactly `BEAMFS_RS_JOURNAL_SIZE = 64` entries of
40 bytes each, occupying 2560 bytes inside the superblock. The
ring-buffer head index is the `s_rs_journal_head` field (`__u8`, modulo
`BEAMFS_RS_JOURNAL_SIZE`). When the buffer wraps, the oldest entry is
overwritten silently - operators are expected to drain the journal
periodically to durable storage if long-term retention is required. The
v4 enlargement (24 → 40 bytes per entry) increased the journal
**information density**, not its entry count.

### 6.3 Entry layout

```c
struct beamfs_rs_event {
    __le64  re_block_no;        /*  0..7   block number or SB sentinel   */
    __le64  re_timestamp;       /*  8..15  ktime_get_ns() at recovery    */
    __le32  re_symbol_count;    /* 16..19  symbols corrected (see §6.4)  */
    __le32  re_entropy_q16_16;  /* 20..23  Shannon H, Q16.16             */
    __le32  re_flags;           /* 24..27  see §6.5                      */
    __le32  re_reserved;        /* 28..31  zero, structural sentinel     */
    __le32  re_crc32;           /* 32..35  CRC32 over bytes [0..32)      */
    __le32  re_pad;             /* 36..39  zero, alignment + sentinel    */
} __packed;
```

Layout invariants enforced at compile time:

- `sizeof(struct beamfs_rs_event) == 40` (BUILD_BUG_ON in `super.c`)
- `re_reserved` and `re_pad` MUST be zero on write
- `re_crc32` covers bytes `[0..32)` - all fields except itself and the
  trailing alignment pad

### 6.4 Symbol count semantics

`re_symbol_count` records the outcome of the Reed-Solomon decode for
the codeword associated with this event:

- `re_symbol_count == 0` AND `re_flags & UNCORRECTABLE == 0` -
  this combination MUST NOT appear in a written entry. It would mean
  "successful decode with zero corrections", which is a non-event and is
  not journaled. Readers MAY treat such an entry as corrupted.

- `0 < re_symbol_count <= BEAMFS_RS_PARITY / 2` (i.e. `1..8`) -
  successful correction of `re_symbol_count` symbols within a single
  codeword. The byte positions of the corrections are summarised by
  `re_entropy_q16_16` (see §6.6) but not retained individually; the
  positions array is consumed at log time.

- `re_symbol_count == 0` AND `re_flags & UNCORRECTABLE != 0` -
  uncorrectable event: the codeword exceeded the RS correction radius
  (more than 8 symbols in error within a single subblock). The data
  could not be recovered, the read returned `-EIO` to userspace, and
  this entry records the location and timestamp for forensic use. See
  §6.5.

- `re_symbol_count > BEAMFS_RS_PARITY / 2` - reserved, MUST NOT be
  written by v4 writers, MUST be treated as corrupted by readers.

### 6.5 Flags

BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID    (1U << 0)
BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE    (1U << 1)


`ENTROPY_VALID` indicates that `re_entropy_q16_16` contains a
mathematically meaningful Shannon estimate. Cleared by zero-init
(mkfs); set by `beamfs_log_rs_event()` when entropy is computed from a
position list of length ≥ 2. See §6.6 for the rationale.

`UNCORRECTABLE` indicates that the codeword exceeded the RS correction
radius. When set:

- `re_symbol_count` MUST be zero (the decoder did not produce a
  correction count for an uncorrectable codeword).
- `ENTROPY_VALID` MUST be cleared (no positions were available to
  compute entropy from).
- `re_block_no` and `re_timestamp` retain their normal meaning, allowing
  the entry to participate in temporal and spatial clustering analyses
  alongside correctable events.

The `UNCORRECTABLE` flag is **orthogonal** to the Family A / Family B
classification of TM §2: an uncorrectable event may originate from
either family and the discrimination is performed post-process by
clustering analysis, not by the kernel. The kernel's role is to record
the raw event with sufficient metadata; classification is userspace.

Bits `(1U << 2)` and higher are reserved and MUST be zero on write.

### 6.6 Entropy estimator

The Shannon entropy of the position list returned by RS decode is the
v4 forensic discriminator between Family A (Poisson background SEU,
TM §2.1) and Family B (correlated burst, TM §2.2).

**Algorithm:**

1. Let `positions[0..n-1]` be the byte positions reported by
   `beamfs_rs_decode()` for a single codeword, with `n =
   re_symbol_count` and `n >= 2`.
2. Quantise each position into one of `BEAMFS_RS_ENTROPY_BINS = 8` bins
   over the codeword length `code_len`:

bin[i] = positions[i] * BEAMFS_RS_ENTROPY_BINS / code_len

3. Build the histogram `h[0..7]` with `sum(h) == n`.
4. Compute the Shannon entropy in Q16.16 fixed point using the
   precomputed lookup table generated by `tools/gen_entropy_lut.py`:

H = - sum_{i: h[i] > 0} (h[i]/n) * log2(h[i]/n)   bits
H_q16_16 = round(H * 65536)


**Range:** `0 <= H <= log2(BEAMFS_RS_ENTROPY_BINS) = 3` bits, so
`re_entropy_q16_16` is in `[0, 3 << 16) = [0, 196608)`.

**Single-sample policy:** when `n == 1`, the entropy is mathematically
defined (`H = 0`) but **not significant** - a single sample carries no
distributional information. To prevent forensic analysts from
misinterpreting these zeros as "evidence of a perfectly clustered
burst", `beamfs_log_rs_event()` clears `ENTROPY_VALID` for `n == 1`.
Per TM §6.4, the analyst then falls back to timestamp clustering for
that entry. This policy is enforced at the kernel boundary and is part
of the on-disk contract.

**LUT reproducibility:** the entropy LUT in `edac.c` is bracketed by
`SENTINEL_LUT_BEGIN` / `SENTINEL_LUT_END` markers and bears the
generator hash. To verify, run `tools/gen_entropy_lut.py` and confirm
the output matches the bracketed region byte-for-byte.

### 6.7 Block number sentinel

The `re_block_no` field encodes the location of the corrected codeword.
Two encoding ranges exist:

- **Data / metadata blocks:** `re_block_no` is a normal block number
  (`< BEAMFS_RS_BLOCK_NO_SB_MARKER`). For data blocks under the
  `UNIVERSAL_INLINE` scheme (§7), the block number is the **logical
  product** `phys_block * BEAMFS_DATA_INLINE_SUBBLOCKS + subblock_idx`,
  encoding both the physical disk block and the subblock within it.
- **Superblock subblocks:** `re_block_no` is the SB sentinel:

if ((re_block_no & BEAMFS_RS_BLOCK_NO_SB_MASK)
== BEAMFS_RS_BLOCK_NO_SB_MARKER)
then sb_subblock_index = re_block_no & BEAMFS_RS_BLOCK_NO_SB_IDX_MASK

  The marker `0xFFFFFFFFFFFFF000ULL` is chosen at the top of the
  `u64` range, well above any realistic block number on a filesystem
  (2^64 blocks × 4 KiB = 64 ZiB, unreachable by current and foreseeable
  storage).

### 6.8 Mount-time recovery flow

This is the "Stage 3 item 4 fill_super event flow" referenced from
`super.c`.

When mounting a v4 volume, the kernel:

1. Reads block 0 into a buffer head.
2. Verifies `s_magic` and `s_version`.
3. Attempts the superblock RS decode (§5) into a 2743-byte staging
   buffer. The decode result is a `pending[]` array of per-subblock
   correction outcomes (no entries, correction with positions, or
   uncorrectable).
4. Verifies `s_crc32`. If mismatch and the RS decode could not correct
   the discrepancy, the mount fails with `-EUCLEAN`.
5. Allocates the in-memory `struct beamfs_sb_info` and links it to the
   buffer head.
6. **Replays** the `pending[]` array through `beamfs_log_rs_event()` -
   each subblock that was corrected at mount produces a journal entry
   with the SB sentinel and the timestamp of the mount. This ensures
   that recovery actions performed before the in-memory journal pointer
   was available are still durably recorded.
7. Sets up the on-disk bitmap (which performs its own RS decode and
   may produce its own journal entries via the same path).
8. Caches `sbi->s_scheme` from `s_data_protection_scheme`.
9. Logs a single `pr_info` line summarising version, block count,
   inode count, scheme, and feature masks.

The replay step is critical: it is the only mechanism by which
mount-time RS corrections become visible to userspace. A failure in
this step is logged but not fatal - the volume mounts and is usable,
but the operator loses the audit record of the mount-time recovery.

---

## 7. Data block protection

### 7.1 Schemes

The `s_data_protection_scheme` field selects the data-block FEC strategy:

NONE = 0 /* no FEC on data; deprecated, TM §6.1 / INODE_OPT_IN = 1 / per-inode FEC opt-in flag; deprecated / UNIVERSAL_INLINE = 2 / per-block inline RS parity; v2 default / UNIVERSAL_SHADOW = 3 / parity in dedicated region; reserved / UNIVERSAL_EXTENT = 4 / parity as extent attribute; reserved / INODE_UNIVERSAL = 5 / legacy iomap path with RS on inode meta */ MAX = 5


The kernel range-checks this field at mount and refuses values above
`BEAMFS_DATA_PROTECTION_MAX`. The unused upper bytes of the `__le32`
act as a structural sentinel: any single-byte corruption in the
high-order bytes produces a value outside the valid range.

### 7.2 UNIVERSAL_INLINE layout (scheme = 2)

Each 4096-byte data block is laid out as **16 interleaved RS(255,239)
shortened subblocks**:

Disk block (4096 bytes): [SB0 data (239) | SB0 parity (16)] = 255 bytes [SB1 data (239) | SB1 parity (16)] = 255 bytes ... [SB15 data (239) | SB15 parity (16)] = 255 bytes [zero pad (16)]

total 4096 bytes

16 * 255 = 4080
4080 + 16 = 4096

Logical user data per disk block: 16 * 239 = 3824 bytes.


Constants:

BEAMFS_DATA_INLINE_SUBBLOCKS = 16
BEAMFS_DATA_INLINE_BYTES     = 16 * 239  = 3824
BEAMFS_DATA_INLINE_TOTAL     = 16 * 255  = 4080
BEAMFS_DATA_INLINE_PAD       = 4096 - 4080 = 16


**Logical-to-physical mapping** for a user file:

iblock_logical = file_offset / 3824
offset_in_logical = file_offset % 3824


A file of size `N` occupies `ceil(N / 3824)` disk blocks. The on-disk
layout is identical regardless of which scheme produced it; the
read/write paths differ in whether they invoke RS encode/decode at the
block boundary.

### 7.3 Correction capacity per disk block

Each subblock tolerates up to 8 symbol errors. Total per-block correction
capacity: **128 byte-symbol errors**, when distributed across all 16
subblocks (8 per subblock). A single subblock that exceeds 8 symbol
errors is uncorrectable regardless of the state of the other subblocks
in the same disk block, and produces an `UNCORRECTABLE` journal entry
per §6.5 with `re_block_no = phys_block * 16 + subblock_idx`.

### 7.4 Read-path recovery

On read of a UNIVERSAL_INLINE data block:

1. Look up the physical block for the requested logical iblock.
2. `sb_bread()` the physical block.
3. Decode all 16 subblocks **in place** via `beamfs_rs_decode_region()`
   with stride `BEAMFS_SUBBLOCK_TOTAL = 255` for both data and parity,
   parity offset 239 within each stride.
4. For each subblock with correction count > 0, journal the event via
   `beamfs_log_rs_event()` with `re_block_no = phys * 16 + i` and the
   position list returned by the decoder.
5. For each subblock with correction count < 0 (uncorrectable), journal
   an UNCORRECTABLE event per §6.5 and propagate `-EIO` to the caller.
   The folio is **not** marked uptodate.
6. If any subblock produced a correction (and no uncorrectables), gather
   the 16 × 239 bytes into the page-cache folio, zero the trailing 272
   bytes (4096 − 3824), and write the **repaired buffer head back to
   disk synchronously** via `mark_buffer_dirty()` + `sync_dirty_buffer()`.
   This is the **autonomic in-place repair** required by TM §6.6.

### 7.5 Write-path RS encoding

The write path (write_begin / write_end / writepages) is described in
the kernel implementation; the on-disk requirement is that any disk
block written under scheme = 2 MUST be encoded as 16 subblocks per the
layout in §7.2 with valid RS parity, otherwise subsequent reads will
trigger spurious correction or uncorrectable events.

---

## 8. Inode layout

The inode structure is unchanged from v3:

```c
struct beamfs_inode {
    __le16  i_mode;
    __le16  i_nlink;
    __le32  i_uid;
    __le32  i_gid;
    __le64  i_size;
    __le64  i_atime;          /* nanoseconds */
    __le64  i_mtime;          /* nanoseconds */
    __le64  i_ctime;          /* nanoseconds */
    __le32  i_flags;
    __le32  i_crc32;          /* CRC32 of inode excluding this field */
    __le64  i_direct[12];     /* direct block pointers */
    __le64  i_indirect;       /* single indirect, ~2 MiB capacity */
    __le64  i_dindirect;      /* double indirect, ~1 GiB capacity */
    __le64  i_tindirect;      /* triple indirect, ~512 GiB capacity */
    __u8    i_pad[...];       /* padding to 256 bytes */
} __packed;
```

Indirect block format: a 4096-byte block of 512 `__le64` block pointers
(`BEAMFS_INDIRECT_PTRS = 4096 / 8 = 512`).

Addressing capacity:
- direct only: 12 × 3824 = 45 KiB (under UNIVERSAL_INLINE)
- single indirect: + 512 × 3824 = + 1.87 MiB
- double indirect: + 512² × 3824 ≈ + 957 MiB
- triple indirect: + 512³ × 3824 ≈ + 478 GiB

The v0.1.0 implementation supports direct + single indirect (~2 MiB per
file). The double and triple indirect fields are present in the
on-disk format for forward compatibility but are not yet exercised by
the kernel module.

---

## 9. Compatibility and migration

beamfs v4 is a fresh format with no backward compatibility to FTRFS or
to internal v2/v3 layouts. Volumes formatted as FTRFS (different magic)
are not mountable as beamfs by design. There is no `fsck.beamfs --upgrade`
path because there are no v3-or-earlier volumes in the wild.

A future v5 format MAY introduce features incrementally via the v3+
feature-bit machinery (§4.3) without requiring a format-version bump,
provided the changes fit within the reserved bits. Format-breaking
changes will produce a v5 specification document.

---

## 10. Forensic analysis (informative)

The following is **informative**, not normative. It documents how
userspace tools and operators are expected to consume the on-disk
journal data; the kernel does not enforce this analysis.

### 10.1 Family A vs Family B discrimination (TM §2.1, §2.2)

Per TM §2, beamfs treats data-at-rest corruption as a single
electromagnetic phenomenon manifesting through two statistically
distinct families:

- **Family A** - stochastic EM perturbations (background SEU,
  thermal stress, retention loss, ambient RF noise). Inter-event
  times follow an exponential distribution; positions within a
  codeword are uniform. Expected entropy per event is high (close to
  `log2(BINS) = 3` bits).
- **Family B** - adversarial EM events (IEMI, HPM, EMP, conducted EMI).
  Inter-event times are clustered; positions within a codeword may be
  either tightly clustered (single point-source) or spread
  (broad-spectrum). Entropy per event varies.

Discrimination procedure for an analyst draining the journal:

1. Sort entries by `re_timestamp`.
2. Compute inter-arrival times and test against an exponential null.
3. Cluster entries by `re_block_no` proximity.
4. Inspect `re_entropy_q16_16` distribution within clusters.
5. Cross-reference UNCORRECTABLE entries (§6.5): a cluster of
   uncorrectable events with low spatial dispersion is a strong
   Family B indicator (saturation boundary reached in a localised
   region; see TM §2.3).

### 10.2 Saturation events

UNCORRECTABLE events (§6.5) record the **saturation boundary** of the
RS protection: a single subblock has exceeded 8 symbol errors. They are
strictly more informative than correctable events for capacity
planning, because they identify the conditions under which the FEC
contract was violated.

A volume that produces UNCORRECTABLE events is operating outside its
designed protection envelope. Operators SHOULD migrate data and
investigate the physical or environmental cause.

### 10.3 Journal exhaustion

The 64-entry ring buffer wraps silently. For long-duration deployments
where the event rate may exceed the drain frequency, operators are
expected to:

- Read the journal at a rate higher than the wrap rate.
- Maintain an off-volume audit log keyed on `(s_uuid, re_timestamp)`.
- Treat journal wrap (detectable by gaps in `re_timestamp` between
  successive drains) as a possible information loss event.

The journal is **not** a guaranteed-delivery audit log. It is a
last-N-events ring buffer suitable for forensic post-mortem and
real-time monitoring with appropriate userspace tooling.

---

## 11. Conformance fixture (canary block)

This section is **normative**. It defines the on-disk conformance
fixture written by `mkfs.beamfs -s inline` and provides the
byte-deterministic SHA256 values that any conforming v4 implementation
MUST reproduce.

### 11.1 Purpose and scope

The conformance fixture is a deterministic data block written to the
`UNIVERSAL_INLINE` (scheme = 2) layout immediately after the root
directory block. Its purpose is to validate the on-disk RS encode
chain end-to-end without relying on user-written content, and to
provide a byte-deterministic regression target for any independent
reimplementation of the beamfs v4 format.

The fixture is **mandatory** for scheme = 2 volumes produced by
`mkfs.beamfs` v0.1.0 and later. It is **absent** for scheme = 5
(`INODE_UNIVERSAL`) and other non-INLINE schemes.

In v4, the fixture exists on-disk only; no VFS alias is exposed
(reserved for v4.1, see `Documentation/roadmap.md`).

### 11.2 On-disk location

```
canary_blk     = bitmap_blk + 2
data_start_blk = bitmap_blk + 3   (under INLINE)
data_start_blk = bitmap_blk + 2   (other schemes, no canary)
```

The canary block occupies one full 4096-byte disk block. The bit
corresponding to `canary_blk` in the on-disk bitmap is set to 0
(allocated). `s_free_blocks` is computed from `data_start_blk`,
which already accounts for the canary block.

### 11.3 User content layout (3824 bytes)

The 3824 user-visible bytes (= `BEAMFS_DATA_INLINE_BYTES`) are
structured as a 64-byte ASCII header followed by a 3760-byte
deterministic payload:

```
offset  0..63    BEAMFS_CANARY_HEADER_LEN = 64 bytes
                 = "beamfs-CANARY-v4 RS(255,239)x16 SHA256-fixed\n"
                   (45 bytes ASCII)
                 + zero pad to 64 bytes
offset 64..3823  BEAMFS_CANARY_PAYLOAD_LEN = 3760 bytes
                 payload[i] = (i ^ (i >> 8)) & 0xFF
                 for i in [0 .. 3759]
```

The payload formula is a XOR-shift fingerprint chosen to exercise
all 256 GF(2^8) symbol values across the codeword while remaining
trivially reproducible without any pseudo-random generator.

### 11.4 On-disk block layout (4096 bytes)

The 3824 user bytes are encoded into 16 interleaved RS(255,239)
shortened subblocks of 255 bytes each (239 user data || 16 parity),
followed by 16 bytes of zero pad:

```
offset    0..254     subblock 0:  user[0..238]   || parity[0..15]
offset  255..509     subblock 1:  user[239..477] || parity[0..15]
...
offset 3825..4079    subblock 15: user[3585..3823] || parity[0..15]
offset 4080..4095    16 bytes zero pad
```

This is identical to the bitmap block layout (`rs_encode_bitmap()`).
The RS encoder is the standard `lib/reed_solomon` Linux kernel
implementation with parameters `init_rs(8, 0x187, fcr=0, prim=1,
nroots=16)`, mirrored byte-for-byte by the userspace mkfs encoder.

### 11.5 Byte-deterministic SHA256 fixtures

Any conforming v4 mkfs implementation MUST produce a canary block
whose SHA256 matches the values below. These values were captured
empirically on 2026-04-29 from two independent builds (host x86_64
gcc and target aarch64 Yocto cross-gcc) and verified byte-for-byte
identical via `cmp(1)`.

```
SHA256 of canary disk block (4096 raw bytes, RS-encoded with parity):
    fb8a3b9e2704ce3fc11a0d0a4139d36c916cb5e7230acde8d2fa00ace739a91c

SHA256 of canary user content (3824 gathered bytes, parity stripped):
    ced446d9bf5e6682a48e8782ce5ad33f575f08921451afaa127f26dc37515bab
```

The disk-block SHA256 is verifiable directly via `dd` without
mounting the filesystem (assuming default mkfs layout with
inode_table_len=16, canary_blk=19):

```
dd if=<image> bs=4096 skip=19 count=1 | sha256sum
```

The user-content SHA256 requires gathering the 16 segments of 239
bytes from the 16 interleaved subblocks (skipping the 16 parity
bytes between each). Once the v4.1 VFS alias is implemented, the
user-content SHA256 will be obtainable directly via
`sha256sum /mnt/canary` after mount.

### 11.6 Reproducibility statement

The fixture is reproducible byte-for-byte across any little-endian
platform with a C99-compliant compiler and the standard `lib/reed_solomon`
Linux kernel implementation (or its mathematically equivalent
userspace counterpart in `mkfs.beamfs::rs_encode_bitmap()`). The
spec depends on:

1. Strict little-endian byte order (`__le32`, `__le64` enforced).
2. `__attribute__((packed))` on all on-disk structures.
3. Standard RS(255,239) shortened over GF(2^8) with primitive
   polynomial `0x187`, fcr=0, prim=1, 16 parity symbols.
4. The deterministic header string and XOR-shift payload formula
   in section 11.3.

Any divergence from these byte-deterministic SHA256 values indicates
either (a) a bug in the implementation under test, or (b) a
deviation from the spec, both of which MUST be diagnosed before
the implementation can claim v4 conformance.

### 11.7 Forensic use

The conformance fixture is also a **forensic baseline**: a verified
canary block on a deployed volume confirms that the on-disk RS
encode chain has not been silently corrupted between mkfs time and
the inspection time. A fixture whose SHA256 has drifted from the
published values without a corresponding journal entry indicates
either:

- An implementation bug in `mkfs.beamfs` or in the kernel write
  path (no scenario in v4 should cause the canary to be re-encoded
  after mkfs).
- A media-level corruption that exceeded the RS correction radius
  and was not journaled (which would itself be a serious bug,
  since `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE` is precisely intended
  to surface such events).
- An adversarial substitution at the block layer (out of scope for
  the v4 threat model; see TM §2.3).

`fsck.beamfs` (future) is expected to verify the fixture SHA256 at
each invocation as a self-test of the on-disk format integrity.

---

## 12. Document maintenance

This document is normative for `BEAMFS_VERSION_V1` v4 layout. Changes
to the on-disk format require a corresponding update to this document
**before** the code change lands, per the beamfs development workflow.
References from the kernel source to this document use the form
`Documentation/format-v4.md` and are expected to remain stable across
the v4 lifetime.

A successor format (v5) will produce a new document
`Documentation/format-v5.md` and v4 will be archived to
`Documentation/historic/format-v4.md` at that time.

---

## 13. References

- `Documentation/threat-model.md` - failure model, certification
  context, design constraints (TM §1–10).
- `Documentation/design.md` - architectural overview, partially
  superseded by this document for v4 specifics.
- `Documentation/known-limitations.md` - explicit non-goals.
- `Documentation/testing.md` - empirical validation methodology.
- `Documentation/EMPIRICAL-FINDINGS.md` - recorded test results.
- `lib/reed_solomon` (Linux kernel) - RS(255,239) implementation.
- `tools/gen_entropy_lut.py` - Shannon LUT generator (reproducible).
