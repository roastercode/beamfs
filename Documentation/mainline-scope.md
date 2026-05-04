# beamfs Mainline Scope

**Status**: scope formalised on the `mainline-prep` branch.
**Last updated**: 2026-04-30.
**Branch**: `mainline-prep`.

---

## 1. Position

beamfs is a read-write Linux filesystem with native inline
Reed-Solomon error correction on the read path. It addresses a
class of silent data corruption that the existing mainline
filesystems do not correct: bit-flips of physical or adversarial
origin reaching the persistent storage layer.

The Linux kernel currently provides Reed-Solomon error correction
only via `dm-fec`, which is read-only and tied to `dm-verity`.
There is no mainline mechanism that performs RS-FEC correction on
read-write storage. beamfs proposes to fill that gap.

This positioning is independent of any specific deployment market.
The threat model addressed by beamfs is universal: any Linux
storage subject to bit-flips of hardware, environmental, or
adversarial origin.

## 2. Threat model

The expanded threat model covered by beamfs includes:

- **SEU (Single-Event Upset)**: a single bit flip caused by an
  ionising particle hitting a memory cell. Present on any silicon,
  in any environment, at a baseline rate amplified by altitude,
  latitude, and proximity to radiation sources.
- **MBU (Multi-Bit Upset)**: multiple bit flips caused by a single
  particle event affecting adjacent memory cells.
- **Bit-rot from aging silicon**: charge loss in NAND flash and
  DRAM cells over time, eventually exceeding internal ECC capacity.
- **IEMI (Intentional Electromagnetic Interference)**: directed EM
  injection used as an attack vector against electronics. Demonstrated
  in academic literature and emerging as a security concern.
- **Voltage glitch attacks**: hardware fault injection used to
  bypass cryptographic boundaries. Attested against TPM, HSM, and
  secure element chips.
- **Rowhammer-class attacks**: software-induced bit flips in DRAM
  via repeated row activations. Affects all DDR3/DDR4/DDR5 memory
  with insufficient mitigation.
- **Environmental radiation**: cosmic rays, terrestrial neutrons,
  solar particle events. Permanent baseline for all systems,
  amplified for spacecraft and high-altitude infrastructure.

These threats produce silent data corruption that:

1. Bypasses CPU ECC if the corruption occurs in the storage path
   between CPU and persistent media.
2. Bypasses storage controller ECC if the corruption occurs in DRAM
   buffers or cache lines.
3. Bypasses dm-integrity CRC if RS-FEC capacity is needed (CRC
   detects, does not correct).
4. Triggers EIO with no automatic recovery in ext4, XFS, btrfs,
   bcachefs, F2FS, and all current mainline read-write filesystems.

Validation of beamfs resilience to this threat model is provided
by the RadFI fault injection harness (companion repository),
exercising bit-flips at sub-block, block, and inode granularity
via `debugfs/radfi` interface.

## 3. Capabilities matrix

The position of beamfs relative to mainline filesystems is the
following. Comparison restricted to features relevant to silent
data corruption resistance.

| Capability                  | ext4 | btrfs | XFS | bcachefs | f2fs | squashfs | EROFS | dm-fec     | beamfs v5 |
|-----------------------------|------|-------|-----|----------|------|----------|-------|------------|-----------|
| Read-write                  | yes  | yes   | yes | yes      | yes  | no (RO)  | no    | no (RO)    | yes       |
| Metadata CRC                | opt  | yes   | yes | yes      | yes  | yes      | yes   | n/a        | yes       |
| Data CRC                    | no   | yes   | opt | yes      | opt  | n/a      | yes   | n/a        | yes       |
| RS-FEC correction           | no   | no    | no  | no       | no   | no       | no    | yes (RO)   | yes (RW)  |
| Auto-repair on read         | no   | RAID1+| no  | replicas | no   | no       | no    | yes (RO)   | yes       |
| Survives single SEU         | no   | det.  | det.| det.     | det. | det.     | det.  | yes (RO)   | yes       |
| Survives MBU within block   | no   | no    | no  | no       | no   | no       | no    | partial    | yes       |
| Mainline                    | yes  | yes   | yes | rejected | yes  | yes      | yes   | yes        | candidate |

`det.` = detection only, returns EIO on read.
`opt` = optional, behind feature flag.

The unique position of beamfs is the intersection of three
properties: (a) read-write, (b) RS-FEC correction, (c) mainline
candidate. No existing component covers this intersection.

## 4. Architecture v5

beamfs v5 uses a single on-disk format with feature flags following
the ext4 pattern (s_feat_compat, s_feat_incompat, s_feat_ro_compat).
Three preconfigured mkfs profiles cover the spectrum of supported
deployments without exposing the individual flags to end users.

### 4.1 Feature flags

The format reserves three categories of feature flags. Compat flags
are informational. RO-compat flags force read-only mount if unknown.
Incompat flags refuse mount if unknown. Bit allocations are listed
in Documentation/format-v5.md.

### 4.2 mkfs profiles

| Profile    | Volume range    | File range     | Latency target  | Active flags                                  |
|------------|-----------------|----------------|-----------------|-----------------------------------------------|
| embedded   | 32 KB - 100 GB  | 4 KB - 1 GB    | hardware-bound  | none (v5.0 minimal)                           |
| server     | 1 GB - 16 EB    | 4 KB - 16 EB   | 50-300 µs       | EXTENTS, 64BIT, BLOCK_GROUPS, JOURNAL         |
| dax        | 64 GB - 100 TB  | 4 KB - 16 EB   | 1-10 µs         | EXTENTS, 64BIT, BLOCK_GROUPS, JOURNAL, DAX    |

Profiles are sets of feature flags, not separate formats. A
filesystem formatted with `--profile=embedded` can be upgraded
in place to `server` or `dax` via `tune.beamfs -O <flags>`,
provided the underlying storage supports the new requirements.

### 4.3 Block size policy

beamfs v5 fixes the on-disk block size at 4096 bytes (page size on
x86-64 and aarch64 kernels in scope). Larger logical clusters are
supported via the BIGALLOC flag (cluster_size = N * BLOCK_SIZE,
N up to 256). This matches the ext4 `bigalloc` design and avoids
the page cache pitfalls of native block_size > PAGE_SIZE.

The beamfs v4 mismatch between user data per block (3824 bytes
INLINE) and PAGE_SIZE (4096 bytes) is resolved in v5 by either:

- option A: keeping the 3824/4096 layout with the multiblock
  read_folio path completed (sub-steps 4-10 of the existing
  multiblock work);
- option B: introducing a v5 INODE_UNIVERSAL scheme where parity
  is stored in a dedicated parity area, restoring 4096 useful
  bytes per disk block.

The decision between A and B is documented in
Documentation/format-v5.md and is independent of the scope
defined in this document.

## 5. Roadmap to mainline

The trajectory toward mainline submission is broken into 8 phases,
documented in Documentation/roadmap.md section "Mainline preparation
roadmap". Phase 0 is this scoping work. Phase 8 is the RFC
submission to linux-fsdevel.

The RFC strategy follows the f2fs and exfat precedents: minimal
viable scope at submission (profile `embedded` only), feature flags
reserved but not all implemented, growth via patch series after
initial acceptance.

## 6. Out of scope

beamfs v5 does NOT provide:

- Native RAID at the filesystem level. Multi-device redundancy is
  delegated to dm-raid, mdraid, or hardware RAID below the FS.
- Copy-on-write snapshots. Delegated to LVM thin or btrfs subvolumes
  if needed at a layer above.
- Native encryption. Delegated to dm-crypt below the FS, or
  fscrypt-style integration in a future v6+ if demand emerges.
- Compression. Considered orthogonal to RS-FEC; may be added as a
  v6+ feature flag.
- Network filesystem semantics. beamfs is a local filesystem.
- MTD/UBI native support. Block device only in v5; UBI integration
  considered for v6+ if demand emerges.

These exclusions are deliberate. The mainline RFC defends a focused
scope: the smallest filesystem that closes the RW RS-FEC gap.
Subsequent features are added by patch series after acceptance.

## 7. Migration

beamfs v5 supports two migration paths:

1. **Copy migration**: `mkfs.beamfs` + `rsync -aHAX` from any
   source filesystem. Always available.
2. **In-place upgrade between profiles**: `tune.beamfs -O +flags`
   on an unmounted beamfs volume. Available within the v5 format.

In-place migration from ext4/btrfs/xfs to beamfs is not provided
in v5. The format incompatibility is too deep to support in-place
conversion without reserving twice the storage.

## 8. References

- Documentation/threat-model.md (current baseline, sections 4, 6)
- Documentation/format-v4.md (current on-disk format)
- Documentation/format-v5.md (target on-disk format)
- Documentation/fsck.beamfs.md (offline checker design)
- Documentation/roadmap.md "Mainline preparation roadmap" section
- linux/Documentation/admin-guide/device-mapper/verity.rst (dm-fec)
- linux/Documentation/filesystems/ext4/overview.rst (feature flags reference)
