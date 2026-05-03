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
under v5 kernels via the migration path of section 9),
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
for read-write mount, with one exception detailed in section 9
(read-only fallback for v4 volumes under a v5 kernel).

### 2.2 Format version vs. kernel module version

- **Format version** (`s_version`, this document): the on-disk
  representation. Mount-blocking when changed (modulo §9).
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

**Note on `BTREE_ALLOC`.** Bit 4 is reserved in the on-disk
inventory but its activation is **deferred to a future format
revision (v6+)**. The reservation prevents collision; the
activation is out of scope for the v5 specification. The chosen
allocator for the v5 server profile is `BLOCK_GROUPS` (bit 3),
which has an established mainline-precedent design (ext4) and
scales to 16 EiB via 64-bit descriptors.

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

## 5. Inode (embedded profile)

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

## 6. Block groups (BLOCK_GROUPS flag)

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

## 7. Extents tree (EXTENTS flag)

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

## 8. DAX-aware layout (DAX flag)

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

## 9. Migration

beamfs v5 supports two migration paths.

### 9.1 Copy migration (always available)

```
mkfs.beamfs --profile=embedded /dev/<target>
mount /dev/<target> /mnt/v5
rsync -aHAXS --numeric-ids /mnt/v4-source/ /mnt/v5/
```

This path is available from any source filesystem (ext4, btrfs,
xfs, beamfs v4) to any beamfs v5 profile. The source is mounted
read-only during the copy; no in-place modification of the source
occurs.

### 9.2 In-place superblock upgrade (v4 to v5 embedded)

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
server or dax MUST use copy migration (§9.1).

### 9.3 Read-only mount of v4 by a v5 kernel

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

## 10. References

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
