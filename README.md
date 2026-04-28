# BEAMFS — Beam-Resilient Filesystem

**Status: PRE-DEMO. Not for publication, not for push, not for Zenodo, not
for kernel.org. This tree exists to validate the recovery calculus of the
companion paper before any public release.**

BEAMFS is the implementation successor to FTRFS. Where FTRFS reaches its
empirical ceiling (per the FTRFS v1 Technical Report,
[doi:10.5281/zenodo.19824442](https://doi.org/10.5281/zenodo.19824442)),
BEAMFS targets the formal recovery calculus described in the BEAMFS v1
Technical Report (under `papers/2026-04-beamfs-v1/`).

## Lineage

The FTRFS name and original concept originates in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: <https://doi.org/10.1007/978-3-319-16086-3_8>

That work was developed at TU Munich (Institute for Astronautics) in the
context of the MOVE-II CubeSat mission. FTRFS v1 (Desbrieres, 2026) is an
independent open-source realization of the Fuchs et al. design with
contemporary Linux kernel infrastructure. BEAMFS v1 (this repository)
extends FTRFS v1 with a formal recovery operator and a soundness theorem
(see `papers/2026-04-beamfs-v1/paper.tex`, Theorem IV.1).

## Repository layout (current)

  - `*.c`, `*.h`             kernel module sources (rebadged from FTRFS v1)
  - `mkfs.beamfs.c`          userspace mkfs tool
  - `Kconfig`, `Makefile`    kernel module build glue
  - `Documentation/`         design notes, threat model, roadmap, testing
  - `tools/`                 helper scripts (decode_raf_journal.py, etc.)
  - `papers/2026-04-beamfs-v1/`
                             LaTeX source of the BEAMFS v1 paper.
                             Build via `cd papers/2026-04-beamfs-v1 && make`.

## Build (kernel module)

The reference target is the Yocto Scarthgap research image at
`~/yocto/poky/build-qemu-arm64/`, which packages
`recipes-kernel/beamfs/` from `~/git/yocto-hardened/`. The `~/git/beamfs/`
tree is the canonical source; it is mirrored byte-exact under
`yocto-hardened/recipes-kernel/beamfs/files/beamfs-0.1.0/` (lockstep).

For a host smoke-test against a Yocto-built kernel tree:

```
make KDIR=<path-to-yocto-kernel-build>
```

Compiling against the host kernel (for example Gentoo 6.18.x) is not
supported and is expected to fail on missing kernel APIs (such as
`inode_state_read_once`, added in mainline 7.0).

## Status flag

- [ ] BEAMFS recovery calculus (Theorem IV.1) implemented in kernel
- [ ] Bench M6 (corrupt-then-mount) implemented and passing
- [ ] Bench M1/M2/M4/M5 reproducing FTRFS baseline within 5%
- [ ] Yocto research image builds, mounts, runs benchmarks
- [ ] Published on Zenodo
- [ ] Published on GitHub (roastercode/beamfs)
- [ ] Submitted to kernel.org / linux-fsdevel

When all checkboxes are ticked, this section is removed and the README
gets a proper public-facing introduction.

## License

GPL-2.0-only (kernel module + userspace tools), CC-BY-4.0 (paper).
See `COPYING` and `papers/2026-04-beamfs-v1/paper.tex` header.
