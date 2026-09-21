# beamfs Current Runtime Architecture

**Snapshot date**: 2026-05-01  
**Validated by**: mega run 20260501-225142 (10/10 phases PASSED)

This document captures the current operational architecture of the
beamfs research lab. For target architecture and design rationale see
`system-architecture.md`. For roadmap see `roadmap.md`.

---

## Host platform

spartian-1 (Gentoo, OpenRC, Wayland/River)
CPU      : 20 cores
RAM      : 31 GiB
Storage  : nvme1n1p2 512 GiB (60% used)
USB      : Bus 001 (USB 2.0 x16 ports), Bus 002 (USB 3.2 Gen 2x2 x9 ports, 20 Gbps)
Toolchain: bitbake (Yocto 7.0), Cargo, GPG ed25519 (key 319A8EAA89C7538AA9550E8BC35EE212519E4857)
Repos    : ~/git/beamfs ~/git/yocto-beamfs ~/git/beamfs-bench


---

## VM cluster (4 nodes, libvirt+QEMU TCG, kernel 7.0.3 aarch64)

+-------------------------------------------------------------------------+
| hpcnet libvirt network 192.168.56.0/24 (isolated, virbr1)               |
|                                                                         |
|   +---------------------------------------+                             |
|   | beamfs-master  192.168.56.10          |                             |
|   |   vCPU=4  RAM=2 GiB                   |                             |
|   |   vda  rootfs squashfs+overlay        |                             |
|   |   vdb  cluster /data on beamfs        |                             |
|   |   role: orchestrator                  |                             |
|   |     - beamfs-bench cli                |                             |
|   |     - cluster_setup driver            |                             |
|   |     - RadFI controller                |                             |
|   |     - forensics aggregator            |                             |
|   |     - GPG manifest signer             |                             |
|   +---------------------------------------+                             |
|                  |                                                      |
|                  | SSH hpcadmin@<IP> with ~/.ssh/hpclab_admin           |
|                  |                                                      |
|     +------------+------------+------------+                            |
|     v                         v            v                            |
|  +-------------------+ +-------------+ +-------------+                  |
|  | beamfs-compute01  | | -compute02  | | -compute03  |                  |
|  | 192.168.56.11     | | .12         | | .13         |                  |
|  | vCPU=4 RAM=2 GiB  | | 4 / 2 GiB   | | 4 / 2 GiB   |                  |
|  | vda rootfs        | | vda rootfs  | | vda rootfs  |                  |
|  | vdb /data beamfs  | | vdb /data   | | vdb /data   |                  |
|  | vdc..vdg 5 USB    | | (no USB)    | | (no USB)    |                  |
|  |   FS-test holder  | |             | |             |                  |
|  | role:             | | role:       | | role:       |                  |
|  |   - cluster mbr   | |   cluster   | |   cluster   |                  |
|  |   - multifs FS    | |             | |             |                  |
|  |   - RadFI target  | |   RadFI tgt | |   RadFI tgt |                  |
|  +-------------------+ +-------------+ +-------------+                  |
+-------------------------------------------------------------------------+


USB layout on compute01 (legacy heterogeneous):

vdc -> ext4     Kingston DataTraveler ...DA006B
vdd -> ext3     Kingston DataTraveler ...D70052
vde -> btrfs    Kingston DataTraveler ...E60058
vdf -> squashfs Kingston DataTraveler ...0ED05
vdg -> beamfs   SanDisk Cruzer ...09503233


---

## Software stack on each VM

+----------------------------------------------------+
| user-space                                         |
|   beamfsd (Electromagnetic Resilience Journal      |
|            daemon, signs peer protocol)            |
|   inject_raf, decode_raf_journal.py                |
|   mkfs.beamfs (v4 format)                          |
|   beamfs-bench worker.sh (deployed at /usr/local/  |
|                            bin/worker.sh)          |
+----------------------------------------------------+
| kernel modules (insmod-loaded, taints kernel)      |
|   beamfs.ko    (out-of-tree, scheme 5 default)     |
|   radfi.ko     (EM fault injection, default off)   |
|   reed_solomon (mainline, beamfs dependency)       |
+----------------------------------------------------+
| kernel 7.0.3 stable mainline                       |
|   built from yocto-beamfs/recipes-kernel/linux/    |
|     linux-mainline_7.0.3.bb + BEAMFS-arm64.cfg     |
|     + multifs.cfg                                  |
+----------------------------------------------------+
| arm64 virtio block + console pl011                 |
+----------------------------------------------------+
| QEMU TCG aarch64 (no KVM, host is x86_64)          |
+----------------------------------------------------+


---

## beamfs-bench scope architecture

beamfs-bench (Rust binary, deployed to /usr/bin/beamfs-bench)
|
+-- pipeline (R19 validation chain, 8 phases 0.0..0.7)
|     -> bitbake beamfs-research-image (setscene cache hit)
|     -> deploy beamfs.ko on 4 VMs in parallel
|     -> verify in-VM ko SHA256 == reference SHA256
|     -> emit GPG-signed manifest.json
|
+-- multifs (5 FS x 3 probabilities)
|     -> per-FS RadFI attack on /dev/vdc..vdg
|     -> verdict per FS x prob (RECOVERED / FS_PANIC / DATA_LOSS)
|
+-- analyse (Full / Quick scope)
|     -> wraps multifs
|     -> adds cluster_attack 4-node parallel on /data
|     -> 4-node forensics capture (dmesg, ftrace, lsmod, radfi-counters)
|     -> generates synthesis.md + cluster-records.txt
|
+-- bitrot (offline FS bit-rot scenarios)
|     -> 4 scenarios: 1 byte / 8 RS limit / 9 over / 256 burst
|
+-- metadata (RadFI metadata block injection)
|     -> 5 FS x 4 scenarios (A1 SB, A2 bitmap, A3 inode, A4 saturation)
|
+-- crash (power-loss mid-write via virsh destroy)
|     -> 5 FS x 1 scenario, post-restart verify
|
+-- fsck (offline FS check)
|     -> 5 FS, fsck.<fs> rc + summary
|
+-- mega (consolidated, 10 phases)
-> capture_env / pipeline_R19 / analyse_full / bitrot / metadata
-> crash / fsck / yocto_build_logs / kernel_artifacts / post_forensics
-> single tarball /tmp/beamfs-bench-mega-<TS>.tar.gz


---

## Lockstep R19 architecture

~/git/beamfs/                          ~/git/yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/
beamfs.h                  ----+ ----+
super.c                       |     |
file_inline.c                 +-->  | byte-identical 12 sources
file.c                        |     | validated by pipeline 0.2
inode.c                       |     |
dir.c                         |     |
namei.c                       |     |
rs.c                          |     |
ioctl.c                       |     |
beamfs_features.h             |     |
beamfs_format.h               |     |
Makefile                  ----+


Both repos must be on synchronized commits before any push. Working
trees must be clean (R19 phase 0.1 enforces). 

---

## Future architecture targets

Two architectures are documented but not yet active:

### Target 1 : 15-USB symmetric HPC (gated by hardware budget)

master:    vda + vdb (slurmctld + munged)
compute01: vda + vdb + vdc..vdg (5 USB homogeneous 16 GB)  + slurmd
compute02: vda + vdb + vdc..vdg (5 USB homogeneous 16 GB)  + slurmd
compute03: vda + vdb + vdc..vdg (5 USB homogeneous 16 GB)  + slurmd


Activates `beamfs-bench hpc` scope: IOR + mdtest under RadFI attack
across 3 nodes in parallel. Requires 15 USB + 15-port USB 3.0 hub.

### Target 2 : Stage 4 data block RS protection (in beamfs.ko)

Extends scheme 5 from "metadata-only RS" to "metadata + data RS".
Three sub-schemes documented in design.md:

scheme 2 = UNIVERSAL_INLINE   (RS parity inline within block)
scheme 3 = UNIVERSAL_SHADOW   (RS parity in dedicated region)
scheme 4 = UNIVERSAL_EXTENT   (RS parity as filesystem attribute)


Decision pending for Stage 4 between these three approaches.
