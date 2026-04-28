# BEAMFS System Architecture

## Positioning

BEAMFS is not a general-purpose filesystem and does not attempt to replace
ext4 or btrfs. It addresses a specific gap in the Linux storage stack:
**in-place correction of silent data corruption on read-write partitions
operating in radiation-intensive environments.**

Understanding where BEAMFS fits requires understanding what the other
components of a hardened embedded Linux system already provide — and
what they do not.

---

## What each layer provides

### dm-verity

dm-verity is a Linux device-mapper target that builds a Merkle tree over
a block device and verifies each block against that tree on read. It
provides cryptographic integrity verification for **read-only** volumes.

Properties:
- Detects any modification to a protected block, including SEU-induced
  bit flips, on read
- Cannot correct errors — a corrupted block returns an I/O error
- Requires the volume to be read-only; any write invalidates the tree
- Widely used for root filesystem protection (Android Verified Boot,
  ChromeOS, embedded Linux secure boot chains)

### squashfs

squashfs is a compressed read-only filesystem. It has no write path
and no allocation state. Combined with dm-verity, it provides a
verified, compressed, immutable root filesystem.

### BEAMFS

BEAMFS provides **in-place error correction** for **read-write** partitions.

Properties:
- Reed-Solomon FEC corrects up to 8 symbol errors per 239-byte subblock
  without operator intervention and without a redundant copy
- Designed for single-device storage (MRAM, NOR flash) where no block
  layer redundancy is available
- Maintains a persistent Radiation Event Journal recording each
  correction event (block number, timestamp, symbols corrected, CRC32)
- Operates on read-write data — not suitable as a read-only filesystem
  integrity mechanism

---

## Why dm-verity and BEAMFS are complementary, not competing

dm-verity and BEAMFS operate on different partition types and address
different failure modes:

| Property              | dm-verity + squashfs   | BEAMFS                    |
|-----------------------|------------------------|--------------------------|
| Access mode           | read-only              | read-write               |
| Failure model         | detect & reject        | detect & correct         |
| Redundancy required   | no                     | no (single device)       |
| SEU response          | I/O error              | in-place RS correction   |
| Persistence of state  | immutable              | mutable, journaled       |
| Use case              | OS, firmware, binaries | mission data, logs, state |

A system that uses only dm-verity has no protection for its read-write
data partition. A system that uses only BEAMFS on the root filesystem
loses the ability to detect tampering of OS binaries. The two
mechanisms address complementary partitions of the storage layout.

---

## Reference architecture for hardened embedded Linux

The following partition layout represents the recommended deployment
of BEAMFS within a complete hardened embedded Linux system:

```
┌─────────────────────────────────────────────────────────────┐
│  Boot device (MRAM / NOR flash / eMMC)                      │
│                                                             │
│  /boot          ext4 or squashfs (read-only)                │
│                 Bootloader, kernel image, device tree       │
│                                                             │
│  /  (rootfs)    squashfs + dm-verity (read-only)            │
│                 OS binaries, libraries, system configuration │
│                 Verified at boot via Merkle tree root hash  │
│                 stored in secure boot chain (TPM / eFuse)   │
│                                                             │
│  /data          BEAMFS (read-write)                          │
│                 Mission data, application state             │
│                 RS FEC: in-place SEU correction             │
│                 Radiation Event Journal: degradation map    │
│                                                             │
│  /var/log       BEAMFS (read-write)                          │
│                 System logs, event records                  │
│                 RS FEC protects log integrity over time     │
└─────────────────────────────────────────────────────────────┘
```

This layout is not theoretical. The BEAMFS Yocto layer
(`github.com/roastercode/yocto-hardened`, branch `arm64-beamfs`) deploys
BEAMFS as a dedicated data partition (`/dev/vdb`) in an arm64 Slurm HPC
cluster, validated on kernel 7.0. The rootfs uses a separate ext4
partition in the current Yocto build; migration to squashfs + dm-verity
for the rootfs is a planned next step.

---

## Comparison with VxWorks HRFS and PikeOS

### VxWorks HRFS

HRFS (High Reliability File System) is a transactional filesystem
providing crash consistency: a write either fully commits or is
fully rolled back. This protects against power-loss scenarios.

HRFS does not provide Reed-Solomon FEC or any mechanism for in-place
correction of SEU-induced bit flips. Radiation protection in VxWorks
deployments is handled at the hardware level (radiation-hardened
processors, TMR circuits) or by redundant storage, not at the filesystem
level.

BEAMFS addresses the case where hardware-level radiation hardening is
either unavailable (COTS SoCs on CubeSats) or insufficient (high-energy
particle events exceeding the hardware LET threshold). It operates at
the filesystem level, below the application, without requiring redundant
storage.

### PikeOS (SYSGO / Thales Group)

PikeOS is a separation-kernel RTOS certified at ECSS Category A
(space) and Common Criteria EAL5+. It provides strong spatial and
temporal isolation between software partitions.

PikeOS filesystem support relies on the underlying RTOS file system
layer, which does not provide filesystem-level SEU correction. Radiation
protection on PikeOS deployments (e.g., Thales Alenia Space Inspire,
GR740-MINI) is provided by the radiation-hardened hardware platform
(LEON4 TMR, Frontgrade GR740).

BEAMFS is not an RTOS and does not compete with PikeOS. On a Linux
partition within a PikeOS mixed-criticality system, BEAMFS could serve
as the read-write data filesystem for the Linux guest, providing
filesystem-level SEU correction that PikeOS itself does not offer.

---

## Current scope and limitations

BEAMFS in its current version is validated as a **data partition**
filesystem. It is not yet suitable as a root filesystem due to the
following missing features:

- **Indirect block support**: files are limited to 48 KiB (12 direct
  blocks). A Linux rootfs requires multi-MiB files.
- **Symlinks**: not yet implemented. Required for a POSIX root
  filesystem.
- **Extended attributes (xattr)**: not yet implemented. Required for
  SELinux, POSIX ACLs, and systemd metadata.
- **Mount read-only enforcement**: `SB_RDONLY` flag is not yet checked
  before writes to the superblock. Planned fix.

These are tracked in [roadmap.md](roadmap.md). The data partition use
case — which is the primary target — is fully operational.

---

## Post-quantum considerations

The current BEAMFS integrity model uses CRC32 (per-block, per-inode,
per-journal-entry) and Reed-Solomon FEC. CRC32 is not a cryptographic
hash and provides no authentication against an adversary with write
access to the storage device.

For environments requiring authenticated metadata integrity against an
active adversary (rather than SEU-induced passive corruption), a future
extension could add per-block HMAC or hash-based signatures
(NIST FIPS 205 SLH-DSA / SPHINCS+) to the inode and superblock
structures. This would provide post-quantum authenticated integrity
while keeping the RS FEC correction layer unchanged.

This extension is out of scope for the current BEAMFS design, which
targets SEU correction in benign (non-adversarial) radiation
environments. It is noted here for completeness and to acknowledge
the direction of the field.
