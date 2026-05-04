# beamfs On-Disk Format Specification (v5)

**Status:** Authoritative specification for `BEAMFS_VERSION_V5` with
the v5.0 minimal superblock layout (RFC submission baseline).

> **Naming clarification.** The kernel version constant is
> `BEAMFS_VERSION_V5 = 5`. The v5 format is a forward-evolution of
> the v4 layout (see `Documentation/format-v4.md`) under a single
> on-disk format parameterised by feature flags following the ext4
> precedent. The RFC-v5.0 baseline activates **zero** feature flags;
> the on-disk byte layout under that baseline is byte-identical to
> v4 except for `s_version`. Subsequent patch series activate flags
> incrementally and define the byte-level layout of the corresponding
> regions at activation time.

**Audience:** Kernel reviewers (linux-fsdevel), certification
auditors, forensic analysts, userspace tooling authors.

**Companion documents:**
`Documentation/format-v4.md` (predecessor specification, normative
for `BEAMFS_VERSION_V1` volumes which remain mountable read-only
under v5 kernels via the migration path of section 13),
`Documentation/threat-model.md` (failure model and adversarial
scope), `Documentation/mainline-scope.md` (architectural goals and
RFC trajectory), `Documentation/roadmap.md` (8-phase mainline
preparation), `Documentation/known-limitations.md` (explicit
non-goals).

This document is the single source of truth for the v5 on-disk
format. All field offsets, sizes, encodings, invariants, and
recovery procedures described here are normative for the v5.0
baseline. Fields, regions, and procedures gated by feature flags
are reserved by this document and specified by the patch series
activating the corresponding flag; until activation they MUST be
zero on write and MUST be ignored on read.

The kernel implementation, the userspace `mkfs.beamfs` formatter,
and any third-party reader MUST conform.

---

## 1. Goals

beamfs v5 satisfies the architectural scope defined in
`Documentation/mainline-scope.md`. Concretely:

- A single on-disk format, parameterised by feature flags following
  the ext4 `s_feat_compat` / `s_feat_incompat` / `s_feat_ro_compat`
  pattern.
- Volume range from 32 KiB (embedded profile floor) to 16 EiB
  (server profile ceiling, via 64-bit block addressing).
- File range from 4 KiB (single-block) to 16 EiB (server profile,
  via extents and 64-bit block numbers).
- Latency target 1-10 microseconds in the `dax` profile, hardware-
  bound elsewhere.
- Mainline-friendly evolution: feature flags reserved at v5.0,
  activated incrementally via patch series after initial RFC
  acceptance.
- Migration via copy (always available) and in-place superblock
  upgrade (within the v5 format).

The RFC-v5.0 baseline (this document) freezes the wire format for
the **embedded profile**: zero feature flags active, byte layout
byte-identical to v4 except for `s_version`. The defensive scope
of v5.0 is documented in section 4 of `mainline-scope.md`.

---

## 2. Format identification and version policy

### 2.1 Magic and version

```
BEAMFS_MAGIC            = 0x4245414D   ("BEAM" little-endian)
BEAMFS_VERSION_CURRENT  = BEAMFS_VERSION_V5 = 5
```

The on-disk `s_magic` and `s_version` fields are the primary
identifiers. `s_magic` is unchanged from v4 and continues to
distinguish beamfs from any predecessor or sibling filesystem.
Mount policy is **strict equality** with `BEAMFS_VERSION_CURRENT`
for read-write mount, with one exception detailed in section 13
(read-only fallback for v4 volumes under a v5 kernel).

### 2.2 Format version vs. kernel module version

- **Format version** (`s_version`, this document): the on-disk
  representation. Mount-blocking when changed (modulo §13).
- **Kernel module version** (`MODULE_VERSION` macro): the
  implementation. May change without bumping the format. The
  format-to-module mapping is recorded in the release notes.

A kernel module that does not recognise the format version refuses
to mount the volume read-write. Forward compatibility is governed
by the feature-flag machinery of section 4.3 and not by the
`s_version` field.

### 2.3 Profile identification

The v5 format defines three mkfs profiles, distinguished by their
feature-flag activation pattern, not by a separate on-disk profile
identifier:

| Profile  | Volume range    | File range    | Latency        | Active flags                                     |
|----------|-----------------|---------------|----------------|--------------------------------------------------|
| embedded | 32 KiB - 100 GiB | 4 KiB - 1 GiB | hardware-bound | none (RFC-v5.0 minimal baseline)                 |
| server   | 1 GiB - 16 EiB   | 4 KiB - 16 EiB | 50-300 us     | EXTENTS + 64BIT + BLOCK_GROUPS + JOURNAL         |
| dax      | 64 GiB - 100 TiB | 4 KiB - 16 EiB | 1-10 us       | embedded flags + DAX                             |

The **embedded** profile is the only profile whose on-disk byte
layout is normatively specified by this document. The `server`
and `dax` profiles are reserved: their flag activation is defined
here (sections 6, 7, 8), but their byte-level layout is the
responsibility of the patch series activating each flag.

---

## 3. Block layout

The block layout in the embedded profile (RFC-v5.0 baseline) is
identical to v4:

```
Block 0            superblock (4096 bytes, magic 0x4245414D)
Block 1..N         inode table (16 inodes per block, 256 bytes per inode)
Block N+1          on-disk block bitmap (RS FEC protected)
Block N+2          conformance fixture (canary block, see format-v4 §11)
Block N+3..end     data blocks (4096 bytes each, UNIVERSAL_INLINE scheme)
```

`N` is determined at format time by `mkfs.beamfs` from the
requested inode count. The default embedded configuration uses
`N=4` (64 inodes total).

When the `BLOCK_GROUPS` flag is active (server profile), the
block layout above is replaced by a multi-block-group layout; the
description of that layout is the responsibility of the patch
series activating `BLOCK_GROUPS`. Until that activation, readers
encountering `BLOCK_GROUPS` set in `s_feat_incompat` MUST refuse
the mount per the rules of section 4.3.

The block bitmap is mandatory in the embedded profile and is itself
protected by Reed-Solomon FEC across 16 subblocks of 239 user data
bytes plus 16 parity bytes per subblock, identical to the data-
block layout described in `format-v4.md` §7.

---

## 4. Superblock (block 0)

### 4.1 Canonical layout (embedded profile, RFC-v5.0 baseline)

The superblock byte layout in the embedded profile is byte-
identical to the v4 layout described in `format-v4.md` §4.1, with
the single exception of the `s_version` field:

```c
struct beamfs_super_block {
    __le32  s_magic;                  /*    0..3   BEAMFS_MAGIC                  */
    __le32  s_block_size;             /*    4..7   always 4096                   */
    __le64  s_block_count;            /*    8..15  total blocks on device        */
    __le64  s_free_blocks;            /*   16..23  free data blocks              */
    __le64  s_inode_count;            /*   24..31  total inodes                  */
    __le64  s_free_inodes;            /*   32..39  free inodes                   */
    __le64  s_inode_table_blk;        /*   40..47  first inode table block       */
    __le64  s_data_start_blk;         /*   48..55  first data block              */
    __le32  s_version;                /*   56..59  BEAMFS_VERSION_CURRENT = 5    */
    __le32  s_flags;                  /*   60..63  reserved (zero)               */
    __le32  s_crc32;                  /*   64..67  CRC32 over coverage regions   */
    __u8    s_uuid[16];               /*   68..83  volume UUID                   */
    __u8    s_label[32];              /*   84..115 volume label, NUL-padded      */
    struct beamfs_rs_event
            s_rs_journal[64];         /*  116..1651 EM Resilience Journal        */
    __u8    s_rs_journal_head;        /* 1652      ring buffer write head        */
    __le64  s_bitmap_blk;             /* 1653..1660 bitmap block number          */
    __le64  s_feat_compat;            /* 1661..1668 informational features       */
    __le64  s_feat_incompat;          /* 1669..1676 incompatible features        */
    __le64  s_feat_ro_compat;         /* 1677..1684 RO-compat features           */
    __le32  s_data_protection_scheme; /* 1685..1688 enum (UNIVERSAL_INLINE)      */
    __u8    s_pad[1383];              /* 1689..3071 padding incl. SB RS parity   */
                                      /* 3888..4095 SB RS parity zone (208 b)    */
} __packed; /* 4096 bytes total, BUILD_BUG_ON enforced */
```

The `s_pad[]` array is **reserved territory** for the v5 format.
In the embedded profile it is zero except for the trailing 208-
byte SB RS parity zone (offsets 3888..4095). When feature flags
are activated, `s_pad[]` is the region from which new fields are
allocated; the byte-level allocation is defined by the patch
series activating each flag. Until then:

- `s_pad[0..1174]` MUST be zero on write.
- `s_pad[1175..1382]` is the SB RS parity zone (208 bytes), see §5.
- Readers MUST NOT interpret non-zero bytes in `s_pad[0..1174]`
  as data; such bytes are CRC-mismatch-inducing and produce a
  failed mount per §4.2.

### 4.2 Coverage and CRC32

`s_crc32` coverage is unchanged from v4. The CRC is computed over
two non-contiguous regions chained via `crc32_le()` without
intermediate XOR:

- **Region A:** `[0, offsetof(s_crc32))` - 64 bytes (header,
  counters, version, flags). Precedes the checksum.
- **Region B:** `[offsetof(s_uuid), offsetof(s_pad))` - 2645 bytes
  (UUID, label, RS journal, journal head, bitmap block, feature
  fields, protection scheme).

Total coverage: **2709 bytes** (`BEAMFS_SB_RS_COVERAGE_BYTES`).
The padding `s_pad[]` is excluded from CRC.

The userspace `mkfs.beamfs::crc32_sb()` MUST produce a byte-
identical CRC32 to the kernel `beamfs_crc32_sb()` for the same
superblock contents, otherwise the volume will fail to mount.

When future patch series move new fields out of `s_pad[]` and into
explicit struct members, the CRC coverage extends to those fields
naturally via `offsetof(s_pad)`. The on-disk contract is preserved
because the staging buffer geometry of §5 is computed from the
coverage range, not hardcoded.

### 4.3 Feature fields

Three 64-bit feature bitmaps are present and consulted at mount
time:

- `s_feat_compat` - informational only. Unknown bits are tolerated.
- `s_feat_incompat` - refuse mount entirely if any unknown bit is
  set.
- `s_feat_ro_compat` - force read-only mount if any unknown bit is
  set.

In `BEAMFS_VERSION_V5` the three masks are zero on a freshly
formatted embedded-profile volume. The bit allocations below are
**reserved**: the bit position is fixed by this document, but the
on-disk byte layout activated by each bit is the responsibility of
the patch series setting the bit for the first time. Until that
patch series merges, mkfs MUST NOT set the bit and the kernel MUST
treat the bit as unknown (refuse mount or force RO per the bitmap
type).

#### 4.3.1 Compat flags (informational, mount continues if unknown)

```c
#define BEAMFS_FEATURE_COMPAT_RS_JOURNAL_VERBOSE  (1ULL << 0)
#define BEAMFS_FEATURE_COMPAT_LABEL_LONG          (1ULL << 1)
#define BEAMFS_FEATURE_COMPAT_DIR_INDEX           (1ULL << 2)
```

| Bit | Flag                                       | Meaning                                          |
|-----|--------------------------------------------|--------------------------------------------------|
| 0   | `BEAMFS_FEATURE_COMPAT_RS_JOURNAL_VERBOSE` | RS event journal records additional metadata     |
| 1   | `BEAMFS_FEATURE_COMPAT_LABEL_LONG`         | Volume label exceeds 32 bytes                    |
| 2   | `BEAMFS_FEATURE_COMPAT_DIR_INDEX`          | Directory hash index for O(log n) lookup         |

#### 4.3.2 RO-compat flags (force RO mount if unknown)

```c
#define BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE       (1ULL << 0)
#define BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE        (1ULL << 1)
#define BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE      (1ULL << 2)
#define BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR        (1ULL << 3)
```

| Bit | Flag                                  | Meaning                                          |
|-----|---------------------------------------|--------------------------------------------------|
| 0   | `BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE` | Files exceed 4 GiB (i_size_high used)            |
| 1   | `BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE`  | Files exceed 16 TiB                              |
| 2   | `BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE`| Inode size exceeds 256 bytes                     |
| 3   | `BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR`  | Directory entries stored in a B-tree             |

#### 4.3.3 Incompat flags (refuse mount if unknown)

```c
#define BEAMFS_FEATURE_INCOMPAT_EXTENTS           (1ULL << 0)
#define BEAMFS_FEATURE_INCOMPAT_64BIT             (1ULL << 1)
#define BEAMFS_FEATURE_INCOMPAT_BIGALLOC          (1ULL << 2)
#define BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS      (1ULL << 3)
#define BEAMFS_FEATURE_INCOMPAT_BTREE_ALLOC       (1ULL << 4) /* see note */
#define BEAMFS_FEATURE_INCOMPAT_JOURNAL           (1ULL << 5)
#define BEAMFS_FEATURE_INCOMPAT_DAX               (1ULL << 6)
#define BEAMFS_FEATURE_INCOMPAT_RS_HEAVY          (1ULL << 7)
#define BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS      (1ULL << 8)
#define BEAMFS_FEATURE_INCOMPAT_BG_RS_PARITY      (1ULL << 9)
#define BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK       (1ULL << 10)
#define BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY     (1ULL << 11)
```

| Bit | Flag                                    | Meaning                                          |
|-----|-----------------------------------------|--------------------------------------------------|
| 0   | `BEAMFS_FEATURE_INCOMPAT_EXTENTS`       | Inode block pointers replaced by extents tree    |
| 1   | `BEAMFS_FEATURE_INCOMPAT_64BIT`         | 64-bit block numbers (above 2^32 blocks)         |
| 2   | `BEAMFS_FEATURE_INCOMPAT_BIGALLOC`      | Block clusters larger than 4 KiB                 |
| 3   | `BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS`  | Multi-bitmap block-group layout                  |
| 4   | `BEAMFS_FEATURE_INCOMPAT_BTREE_ALLOC`   | B-tree allocator (reserved, deferred to v6+)     |
| 5   | `BEAMFS_FEATURE_INCOMPAT_JOURNAL`       | Metadata write-ahead journal                     |
| 6   | `BEAMFS_FEATURE_INCOMPAT_DAX`           | DAX-aware on-disk layout                         |
| 7   | `BEAMFS_FEATURE_INCOMPAT_RS_HEAVY`      | RS(255,191) per subblock (heavier protection)    |
| 8   | `BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS`  | Per-inode RS scheme override                     |
| 9   | `BEAMFS_FEATURE_INCOMPAT_BG_RS_PARITY`  | Parity computed per block group                  |
| 10  | `BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK`   | Block size larger than `PAGE_SIZE`               |
| 11  | `BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY` | Out-of-band RS parity, stride-placed (v5.x)      |

**Note on `BTREE_ALLOC`.** Bit 4 is reserved in the on-disk
inventory but its activation is **deferred to a future format
revision (v6+)**. The reservation prevents collision; the
activation is out of scope for the v5 specification. The chosen
allocator for the v5 server profile is `BLOCK_GROUPS` (bit 3),
which has an established mainline-precedent design (ext4) and
scales to 16 EiB via 64-bit descriptors.

**Note on `PER_INODE_RS`.** Bit 8 activates RS(255,239) protection
on all inodes regardless of `s_data_protection_scheme`. When set,
mkfs.beamfs MUST compute and write per-inode parity at format time
into the 16-byte parity region of each inode (`i_reserved[0..15]`,
at offset 156..171, covering `BEAMFS_INODE_RS_DATA = 172` bytes of
inode payload). At mount time, the kernel applies RS decoding on
any inode whose CRC32 mismatches, and re-verifies CRC32 on the
corrected buffer before accepting the inode. The flag is
composable with `s_data_protection_scheme = UNIVERSAL_INLINE` (2)
to obtain combined data-block + inode RS coverage; it is also
composable with `INODE_UNIVERSAL` (5) where it is functionally
redundant (scheme 5 already enforces inode RS unconditionally).
Reverting a volume formatted with this bit set requires offline
rewrite of every inode block; in-place clear is not supported.

Bits 11 and higher in each bitmap are reserved and MUST be zero
on write.

#### 4.3.4 Mount-time enforcement

At mount time, after CRC32 validation succeeds, the kernel:

1. Reads `s_feat_incompat` and computes the bitwise AND with the
   complement of the kernel's known-incompat mask. If non-zero,
   the mount fails with `-EINVAL` and a `pr_err` line lists the
   unrecognised bit positions.
2. Reads `s_feat_ro_compat` and computes the same AND with the
   known-ro-compat mask. If non-zero, the mount succeeds in
   read-only mode and a `pr_warn` line lists the bits forcing RO.
3. Reads `s_feat_compat` and ignores unknown bits with a `pr_info`
   line for the operator.

The known-incompat and known-ro-compat masks are the OR of the bits
whose activating patch series have merged. In RFC-v5.0 both masks
are zero (no flag is implemented), so any non-zero feature bitmap
fails the mount or forces RO.

---

## 5. Superblock Reed-Solomon protection

NOTE: The byte layout in the v5 embedded baseline is identical to v4 (no geometry change); this section reproduces the normative specification for self-containment.


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

When mounting a v5 embedded volume, the kernel:

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

## 8. Conformance fixture (canary block)

This section is **normative**. It defines the on-disk conformance
fixture written by `mkfs.beamfs -s inline` and provides the
byte-deterministic SHA256 values that any conforming v5 embedded implementation
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
(reserved for v5.x, see `Documentation/roadmap.md`).

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

Any conforming v5 embedded mkfs implementation MUST produce a canary block
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
bytes between each). Once the v5.x VFS alias is implemented, the
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
the implementation can claim v5 embedded conformance.

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
  the v5 threat model; see TM §2.3).

`fsck.beamfs` (future) is expected to verify the fixture SHA256 at
each invocation as a self-test of the on-disk format integrity.

---


**v5 baseline note.** The on-disk canary header string remains
`BEAMFS-CANARY-v4 RS(255,239)x16 SHA256-fixed\n` under the v5
embedded baseline (RFC-v5.0). This is by design: §1 of this document
specifies that the v5 embedded byte layout is identical to v4 except
for `s_version`. Preserving the canary header string preserves the
two SHA256 conformance fixtures (`fb8a3b9e...` disk-block,
`ced446d9...` user-content) byte-for-byte across v4 and v5 embedded
volumes, which is part of the empirical resilience evidence cited in
the beamfs scientific publication.

## 9. Inode (embedded profile)

The inode structure in the embedded profile is byte-identical to
v4 (see `format-v4.md` §8) and is reproduced here for self-
containment:

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
    __u8    i_reserved[84];   /* padding to 256 bytes; first 16 hold RS parity */
} __packed; /* 256 bytes total, BUILD_BUG_ON enforced */
```

Indirect block format and addressing capacity are unchanged from
v4. The v0.1.x kernel implementation supports direct + single
indirect (~2 MiB per file) under the embedded profile. The double
and triple indirect fields are present in the on-disk format but
are not yet exercised; they will be reachable via the kernel
implementation in a future v0.1.x update without a format bump.

The `i_reserved[84]` region is partitioned: the first 16 bytes
(`BEAMFS_INODE_RS_PAR`) hold the RS parity bytes for the inode-
level integrity check; the remaining 68 bytes are reserved territory
for v5 feature flags. When `EXTRA_ISIZE` (RO-compat bit 2) or
`EXTENTS` (incompat bit 0) are activated by future patch series,
their on-disk allocation comes out of these 68 bytes (with
`EXTRA_ISIZE` allowing inode size > 256 bytes via additional
blocks per inode-table block).

Until those flags are activated by their respective patch series:

- `i_reserved[0..15]` is the RS parity zone (16 bytes).
- `i_reserved[16..83]` MUST be zero on write.
- Readers MUST NOT interpret non-zero bytes in `i_reserved[16..83]`
  as data.

---

## 10. Block groups (BLOCK_GROUPS flag)

This section is **reserved**. The bit allocation
`BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS = (1ULL << 3)` is fixed by
section 4.3.3 of this document. The semantic role of the flag is:

- When set, the device is partitioned into multiple block groups,
  each carrying its own block bitmap, inode bitmap, and inode
  table. The single-bitmap layout of section 3 is replaced by a
  group-descriptor table located immediately after the superblock.
- Block group size, descriptor size, descriptor table location,
  and the relationship between `s_block_count` and the per-group
  block count are specified by the patch series activating
  `BLOCK_GROUPS`.

The reference design follows the ext4 block-group layout pattern,
adapted to beamfs invariants:

- 4096-byte block size enforced (no `BIGALLOC` interaction in v5).
- Each block group's bitmap is RS-FEC protected per the existing
  bitmap layout (`format-v4.md` §7.1 with `BEAMFS_BITMAP_SUBBLOCKS
  = 16`).
- Group descriptor integrity is protected by per-descriptor CRC32
  and by RS-FEC across the descriptor table.

The byte-level layout of the block group descriptor and the
descriptor table is **out of scope** for the v5.0 baseline and
will be specified by the patch series activating
`BLOCK_GROUPS` (server profile).

A v5.0-baseline kernel encountering `BLOCK_GROUPS` set in
`s_feat_incompat` MUST refuse the mount per §4.3.4 step 1.

---

## 11. Extents tree (EXTENTS flag)

This section is **reserved**. The bit allocation
`BEAMFS_FEATURE_INCOMPAT_EXTENTS = (1ULL << 0)` is fixed by
section 4.3.3. The semantic role of the flag is:

- When set, the inode block pointer fields
  (`i_direct[12]`, `i_indirect`, `i_dindirect`, `i_tindirect`,
  total 120 bytes) are reinterpreted as the root of an extents
  tree, fitting four in-inode extent records plus an extent
  header.
- Extent records use 48-bit logical and physical block numbers
  and 16-bit extent length, scaling to 16 TiB per extent under
  4 KiB blocks.
- When `64BIT` (incompat bit 1) is also set, extent records
  expand to 64-bit physical block numbers, scaling to 16 EiB.

The reference design follows the ext4 extents layout
(`Documentation/filesystems/ext4/dynamic.rst` "Extent Tree" section),
adapted to beamfs invariants:

- Each extent tree node is a single 4 KiB block, RS-FEC protected
  per the existing block-bitmap layout pattern.
- Extent header, intermediate nodes (index entries), and leaf
  nodes (extent entries) follow the ext4 byte layout for source-
  level review compatibility, with magic and checksum fields
  beamfs-specific.

The byte-level layout of the extent tree node, header, index
entry, and extent entry is **out of scope** for the v5.0
baseline and will be specified by the patch series activating
`EXTENTS` (server profile).

A v5.0-baseline kernel encountering `EXTENTS` set in
`s_feat_incompat` MUST refuse the mount per §4.3.4 step 1.

---

## 12. DAX-aware layout (DAX flag)

This section is **reserved**. The bit allocation
`BEAMFS_FEATURE_INCOMPAT_DAX = (1ULL << 6)` is fixed by section
4.3.3. The semantic role of the flag is:

- When set, the volume targets persistent-memory devices (pmem,
  CXL Type-3) accessed via `DAX` (Direct Access) without page
  cache intermediation.
- The mount path requires `block_size == PAGE_SIZE` strictly.
  Any deviation produces `-EINVAL`. The `LARGE_BLOCK` flag is
  mutually exclusive with `DAX` in the v5 format.
- The mmap path uses `vm_operations_struct` with DAX-aware fault
  and huge_fault callbacks and bypasses the page cache. RS decode
  occurs at access time on the user-mapped folio, not at folio
  load time, because under DAX the folio IS the on-device backing.
- The write path uses `dax_iomap_rw()` and produces RS encode on
  the user-supplied buffer before persisting via
  `arch_wb_cache_pmem()` / `pmem_wmb()` barriers.

The reference design follows the ext4 DAX integration
(`fs/ext4/inode.c` `ext4_iomap_ops` and friends), adapted to
beamfs invariants:

- Inline RS parity layout per block (16 subblocks of 239+16
  bytes, 16-byte trailing pad) is preserved.
- The 3824 logical bytes per disk block are mmap-visible only
  through the gather step performed during access; the raw 4096-
  byte device block is never mapped to userspace.

The byte-level layout details and the kernel-side IO path are
**out of scope** for the v5.0 baseline and will be specified by
the patch series activating `DAX` (dax profile).

A v5.0-baseline kernel encountering `DAX` set in
`s_feat_incompat` MUST refuse the mount per §4.3.4 step 1.

---

## 13. Migration

beamfs v5 supports two migration paths.

### 13.1 Copy migration (always available)

```
mkfs.beamfs --profile=embedded /dev/<target>
mount /dev/<target> /mnt/v5
rsync -aHAXS --numeric-ids /mnt/v4-source/ /mnt/v5/
```

This path is available from any source filesystem (ext4, btrfs,
xfs, beamfs v4) to any beamfs v5 profile. The source is mounted
read-only during the copy; no in-place modification of the source
occurs.

### 13.2 In-place superblock upgrade (v4 to v5 embedded)

A v4 volume can be upgraded in place to v5 embedded profile by
the userspace tool `tune.beamfs --upgrade-format`. The on-disk
preconditions for in-place upgrade are:

- The source volume MUST be unmounted.
- The source volume MUST have `s_data_protection_scheme ==
  BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE` (scheme 2).
- The source volume MUST have `s_feat_incompat == 0`,
  `s_feat_ro_compat == 0`, `s_feat_compat == 0`. (No feature
  bits, since v4 defines no bit allocations.)

The upgrade procedure is:

1. Read the v4 superblock, verify CRC32 and RS.
2. Set `s_version = BEAMFS_VERSION_V5`.
3. Recompute `s_crc32` over the same coverage range (§4.2).
4. Recompute the SB RS parity over the same staging range
   (§5 of `format-v4.md`).
5. Write the superblock back.

The data, inode, bitmap, and canary blocks are unchanged. The
upgraded volume is byte-identical to the v4 source except for
`s_version`, `s_crc32`, and the SB RS parity zone. In particular,
the on-disk RS journal is preserved across the upgrade and
remains forensically valuable.

In-place upgrade from v5 embedded to v5 server or v5 dax
(activating `BLOCK_GROUPS`, `EXTENTS`, `64BIT`, `JOURNAL`, or
`DAX`) is **not supported**: those flag activations require
restructuring the on-disk layout in ways that cannot be performed
without full data movement. Operators upgrading from embedded to
server or dax MUST use copy migration (§13.1).

### 13.3 Read-only mount of v4 by a v5 kernel

A v5 kernel encountering an unmodified v4 superblock
(`s_version == BEAMFS_VERSION_V1 == 1`) MAY mount the volume
read-only. This is the only deviation from the strict version-
equality mount policy of §2.1 and exists to allow a v5 kernel to
read a v4 source for the duration of a copy migration without
requiring a separate v4-only kernel build.

A v5 kernel MUST NOT mount a v4 volume read-write under any
circumstance: read-write access to a v4 volume requires the v4
kernel module.

---

## 14. References

- `Documentation/format-v4.md` - predecessor specification,
  normative for `BEAMFS_VERSION_V1` volumes.
- `Documentation/threat-model.md` - failure model, certification
  context, design constraints (TM §1-10).
- `Documentation/mainline-scope.md` - architectural goals and RFC
  trajectory (sections 4 "Architecture v5", 5 "Roadmap to
  mainline", 6 "Out of scope", 7 "Migration").
- `Documentation/roadmap.md` - 8-phase mainline preparation
  roadmap.
- `Documentation/known-limitations.md` - explicit non-goals.
- `Documentation/testing.md` - empirical validation methodology.
- `Documentation/fsck.beamfs.md` - offline checker design.
- `lib/reed_solomon` (Linux kernel) - RS(255,239) implementation.
- `Documentation/filesystems/ext4/overview.rst` (Linux kernel) -
  feature-flag pattern reference.
- `Documentation/filesystems/ext4/dynamic.rst` (Linux kernel) -
  extents tree layout reference.
- `Documentation/process/coding-assistants.rst` (Linux kernel) -
  AI-assistance disclosure policy applicable to this document.
