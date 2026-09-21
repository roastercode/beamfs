# beamfs - resilient filesystem

beamfs is an EM-resilient Linux filesystem with RS(255,239) forward
error correction, targeting mainline Linux RFC submission. It is the
implementation successor to FTRFS, extending the original design with
a formal recovery calculus and a soundness theorem.

## Current state (2026-07-08)

The kernel module builds, mounts as rootfs, and passes the full R19
validation pipeline (`beamfs-bench full`) under live RadFI electromagnetic
fault injection on a 4-node aarch64 QEMU cluster (kernel 7.0.9 mainline).
The §3.10 evict_inode bug (BUG_ON in clear_inode) is closed as of this
date. Two known limitations remain open: §3.11 (RS silent miscorrection
on data blocks under high-density injection) and §3.12 (emufi disabled
on compute01).

Published artifacts:
- beamfs v2 paper: [DOI 10.5281/zenodo.19886192](https://doi.org/10.5281/zenodo.19886192)
- emufi v1 paper: [DOI 10.5281/zenodo.19885777](https://doi.org/10.5281/zenodo.19885777)
- FTRFS v1 (predecessor): [DOI 10.5281/zenodo.19824442](https://doi.org/10.5281/zenodo.19824442)
- ORCID: [0009-0002-0912-9487](https://orcid.org/0009-0002-0912-9487)

## Lineage

The FTRFS name and original concept originates in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: <https://doi.org/10.1007/978-3-319-16086-3_8>

That work was developed at TU Munich (Institute for Astronautics) in the
context of the MOVE-II CubeSat mission. FTRFS v1 (Desbrieres, 2026) is an
independent open-source realization of the Fuchs et al. design with
contemporary Linux kernel infrastructure. beamfs extends FTRFS with a
formal recovery operator and a soundness theorem (Theorem IV.1, split
into v2.2a proven + v2.2b conjecture in the v2 paper).

## Repository layout

  - `*.c`, `*.h`             kernel module sources
  - `Kconfig`, `Makefile`    kernel build glue
  - `tools/`                 fsck.beamfs, helper scripts
  - `Documentation/`         design notes, known limitations, roadmap
  - `papers/`                LaTeX sources (v1, v2, v3-findings)
  - `context/`               session context, patches archive

## Build

The reference target is the Yocto styhead research image built by
`~/git/yocto-beamfs/` (layer `yocto-beamfs`), producing
`beamfs-research-image-qemuarm64.beamfs` for the 4-VM aarch64
cluster. The `~/git/beamfs/` tree is the canonical source; it is
mirrored byte-exact under `yocto-beamfs/recipes-kernel/beamfs/files/
beamfs-0.1.3/` (lockstep, enforced by R9/R19 pipeline).

Validation kernel: linux-stable `linux-7.0.y` branch, currently 7.0.9.
Host: Gentoo amd64, kernel 7.1.3 (cross-compiles via Yocto).

For a host smoke-test against a Yocto-built kernel tree:

make KDIR=<path-to-yocto-kernel-build>


Compiling against a desktop kernel (e.g. Gentoo 7.1.x) is not supported
and will fail on missing APIs (`inode_state_read_once`, mainline 7.0+).

## Companion repositories

- [yocto-beamfs](https://github.com/roastercode/yocto-beamfs) - Yocto layer (lockstep mirror, cluster config, HPC stack)
- [beamfs-bench](https://github.com/roastercode/beamfs-bench) - Rust validation pipeline (R0/R19, multifs bench, RadFI orchestration)
- [emufi](https://github.com/roastercode/emufi) - EM fault injector (kprobe-based, bio-layer)
- [beamfs-overlay](https://github.com/roastercode/beamfs-overlay) - Gentoo ebuild overlay

## Status

- [x] Yocto research image builds, mounts rootfs=beamfs, boots 4-node cluster
- [x] R19 bench pipeline (`beamfs-bench full`) passes under RadFI injection
- [x] §3.10 evict_inode BUG_ON closed (invalidate_inode_buffers, 2026-07-08)
- [x] Published on Zenodo (beamfs v2 + emufi v1)
- [ ] beamfs recovery calculus (Theorem v2.2b) proven
- [ ] §3.11 RS silent miscorrection on data blocks resolved
- [ ] §3.12 emufi re-enabled on compute01
- [ ] RFC submitted to linux-fsdevel
- [ ] Published on GitHub (public, roastercode/beamfs)

## License

GPL-2.0-only (kernel module + userspace tools), CC-BY-4.0 (papers).
See `COPYING` and paper headers.
