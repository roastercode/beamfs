# BEAMFS v1 — Empirical Findings (2026-04-28)

This document records what was empirically validated and what was empirically
**falsified** during the validation session of 2026-04-28. It is the canonical
honest record of BEAMFS v1 implementation status, independent of paper claims.

## Validated

### Filesystem format
- On-disk magic confirmed: `4D 41 45 42` little-endian = `0x4245414D` ("BEAM")
- Format v1, scheme=5 (INODE_UNIVERSAL), no incompat features
- 16384 blocks layout: superblock (0), inode table (1), bitmap (17),
  data start (19)

### Module loading
- `beamfs.ko` loads successfully on Linux 7.0.1 arm64
- Hard dependency on `reed_solomon.ko` (must be loaded first; no auto-resolve
  via modules.dep at this stage)

### Mount / I/O
- `mkfs.beamfs` creates valid v1 layout
- Mount type=beamfs on /dev/loop or /dev/vdb succeeds
- Read/write/stat work correctly
- Slurm cluster operational across 4 VMs (1 master + 3 compute)

### Performance parity (M1-M5 vs FTRFS baseline)
| Metric | FTRFS baseline | BEAMFS |
|---|---|---|
| M1 write+fsync (MB/s, median) | 5.000 | 5.000 |
| M2 read cold (MB/s, median) | 20 | 20 |
| M4 stat bulk (s, median) | 0.150 | 0.150 |
| M5 small write (ms, median) | 23.5 | 22 |

Performance migration FTRFS → BEAMFS = functionally neutral.

## Falsified

### Theorem IV.1 scope (data blocks)
**Empirically falsified using RadFI v0.1.0 (kernel module fault injector,
kprobe on `submit_bio_noacct`):**

Setup:
- 64MiB BEAMFS image on tmpfs loop device
- Single 40KiB user file written with `dd ... conv=fsync`
- RadFI configured: `hook_blk=1`, `probability=1000000` (100%)

Result:
- 17 bio submissions intercepted by RadFI (`call_count=17`)
- 15 single-bit flips applied to bio_vec page payloads (`flip_count=15`)
- File hash differs from original (one byte at offset 1184: 0xb4 → 0xb0)
- Kernel `dmesg` shows **no recovery attempt, no error log**
- `drop_caches` confirms corruption is physically on disk
- BEAMFS reads corrupted data without warning

**Conclusion:** BEAMFS v1 does **not** apply Reed-Solomon FEC to user data
blocks. Only the bitmap (block 17) is annotated as "RS FEC protected" by
`mkfs.beamfs`. Theorem IV.1 as stated in the paper applies to bitmap
metadata only in v1.

### Implication
- Lecture B confirmed: BEAMFS v1 is a mechanical rebadge of FTRFS v1
  with RS FEC scoped to bitmap metadata
- Lecture A (full filesystem-level RS FEC) is the BEAMFS v2 roadmap target
- Paper Theorem IV.1 must be re-stated to reflect this scope

## Untested / Pending

### Bitmap recovery (block 17)
The narrow Theorem IV.1 (RS recovery on bitmap corruption) was attempted with
the legacy `inject_raf` tool inherited from FTRFS, but `inject_raf` is a
**simulation tool that writes a 40-byte RAF event into the superblock journal
without recalculating the global SB CRC32**. The mount fails with
"superblock CRC32 mismatch and RS uncorrectable" — but this does not test
bitmap recovery, it tests SB consistency.

A proper test would use RadFI scoped to block 17 with `target_dev` filtering.
Pending for v1.1 paper revision.

## Tooling artifacts

- BEAMFS source: `~/git/beamfs/`
- BEAMFS Yocto layer: `~/git/yocto-beamfs/`
- BEAMFS image: `hpc-arm64-research-beamfs-qemuarm64.squashfs`
- VMs: `beamfs-{master,compute01,compute02,compute03}` on hpcnet
  192.168.56.10/11/12/13
- Snapshot: `~/backup-beamfs/beamfs-m1-m5-baseline-ok--20260428-223654.tar.gz`
- Snapshot: `~/backup-beamfs/beamfs-radfi-scaffold-ok--20260428-225935.tar.gz`

## Author

Aurélien Desbrières — Independent researcher.
ORCID: 0009-0002-0912-9487
