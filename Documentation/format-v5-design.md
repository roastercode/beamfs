# BEAMFS On-Disk Format v5 — Design Document

**Status**: skeleton, populated phase by phase.
**Last updated**: 2026-04-30.
**Branch**: `mainline-prep`.

---

## 1. Goals

BEAMFS v5 satisfies the scope defined in
Documentation/mainline-scope.md. Concretely:

- Single on-disk format, parameterised by feature flags.
- Volume range 32 KB to 16 EB across profiles.
- File range 4 KB to 16 EB across profiles.
- Latency target 1-10 µs in profile `dax`, hardware-bound elsewhere.
- Mainline-friendly evolution via incompat / ro_compat / compat
  feature flags, following the ext4 pattern.
- Migration via copy and in-place flag upgrade.

## 2. Format identification

| Field        | v4 value          | v5 value          |
|--------------|-------------------|-------------------|
| s_magic      | 0x4245414D ("BEAM")| 0x4245414D (kept) |
| s_version    | 4                 | 5                 |
| Block size   | 4096 fixed        | 4096 fixed        |

## 3. Feature flags inventory

### 3.1 Compat flags (informational, mount continues if unknown)

TBD Phase 1.0. Reserved bit allocations only at this stage.

| Bit | Flag                          | Description                |
|-----|-------------------------------|----------------------------|
| 0   | BEAMFS_FEATURE_COMPAT_RS_JOURNAL_VERBOSE | RS event journal verbose|
| 1   | BEAMFS_FEATURE_COMPAT_LABEL_LONG | Volume label > 32 bytes |
| 2   | BEAMFS_FEATURE_COMPAT_DIR_INDEX  | Directory hash index    |

### 3.2 RO-compat flags (force RO mount if unknown)

TBD Phase 1.X. Reserved bit allocations.

| Bit | Flag                                | Description                |
|-----|-------------------------------------|----------------------------|
| 0   | BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE | files > 4 GB               |
| 1   | BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE  | files > 16 TB              |
| 2   | BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE| inodes > 256 bytes         |
| 3   | BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR  | dir entries in btree       |

### 3.3 Incompat flags (refuse mount if unknown)

TBD Phase 1.X. Reserved bit allocations.

| Bit | Flag                                  | Description                |
|-----|---------------------------------------|----------------------------|
| 0   | BEAMFS_FEATURE_INCOMPAT_EXTENTS       | extents instead of d/i/di  |
| 1   | BEAMFS_FEATURE_INCOMPAT_64BIT         | 64-bit block numbers       |
| 2   | BEAMFS_FEATURE_INCOMPAT_BIGALLOC      | block clusters > 4 KB      |
| 3   | BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS  | multi-bitmap block groups  |
| 4   | BEAMFS_FEATURE_INCOMPAT_BTREE_ALLOC   | btree allocator            |
| 5   | BEAMFS_FEATURE_INCOMPAT_JOURNAL       | metadata journal           |
| 6   | BEAMFS_FEATURE_INCOMPAT_DAX           | DAX-aware on-disk format   |
| 7   | BEAMFS_FEATURE_INCOMPAT_RS_HEAVY      | RS(255,191) per subblock   |
| 8   | BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS  | per-inode RS scheme override |
| 9   | BEAMFS_FEATURE_INCOMPAT_BG_RS_PARITY  | parity per block group     |
| 10  | BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK   | block_size > PAGE_SIZE     |

## 4. Superblock v5 layout

TBD Phase 1.0. The v4 superblock structure is the starting point.
Changes for v5:

- s_version field bumped to 5.
- s_feat_* fields populated with non-zero values when flags are
  activated.
- Reserved fields for block group count, btree root, journal area
  pointer, when corresponding flags are active.

## 5. Inode v5 layout

TBD Phase 1.X. The v4 inode is 256 bytes. Changes for v5:

- When EXTENTS flag active: i_direct[12] / i_indirect / i_dindirect
  fields are reinterpreted as extents tree root, fitting in 60 bytes.
- When EXTRA_ISIZE flag active: inode size > 256 bytes (xattr space,
  extra timestamps, RS-heavy parity).

## 6. Block groups (BLOCK_GROUPS flag)

TBD Phase 1.X.

## 7. Multi-bitmap chained (alternative to block groups)

TBD Phase 1.X. Decision pending: block groups (ext-style) versus
chained bitmaps (simpler) versus btree allocator (scales to 16 EB).

## 8. Extents tree (EXTENTS flag)

TBD Phase 1.X.

## 9. BTREE allocator (BTREE_ALLOC flag)

TBD Phase 1.X.

## 10. DAX-aware layout (DAX flag)

TBD Phase 1.X. Constraints:

- block_size = PAGE_SIZE strictly.
- mmap path bypasses page cache.
- RS decode happens at access, not at folio load.

## 11. Migration v4 to v5

TBD Phase 1.X. Two paths:

- copy migration: `mkfs.beamfs` v5 + rsync from mounted v4 source.
- in-place: `tune.beamfs --upgrade-format` updates the superblock
  s_version field if the v4 layout is compatible with the requested
  v5 flags.

## 12. References

- Documentation/format-v4.md (current baseline)
- Documentation/mainline-scope.md (architectural goals)
- Documentation/roadmap.md "Mainline preparation roadmap" section
- linux/Documentation/filesystems/ext4/overview.rst (feature flags pattern)
