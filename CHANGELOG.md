# Changelog

All notable changes to beamfs are recorded here: the kernel module, its
on-disk format and the userspace tools in `tools/`. Versions are those
of the module (`MODULE_VERSION`); the tools keep their own version
numbers, named in an entry when they change. The layout follows Keep a
Changelog (https://keepachangelog.com/en/1.1.0/), with one section of
its own, Known defects.

This record starts at 0.1.26, the version the v3 report measures; the
history before it is in `git log`.

## [Unreleased]

### Added

- This changelog. The README carries its latest entries at the top, and
  the warning on the known defects of 0.1.26 is now one of them.

## [0.1.26] - 2026-10-02

The version the v3 report measures (commit `1bf151d`).

### Known defects

A code review on 2026-10-09 found these defects in 0.1.26 and in every
earlier version. They were established by reading the code; each fix
in 0.2.0 comes with a test that reproduces the defect. Until a release
says otherwise, do not keep data you cannot afford to lose on beamfs,
do not mount an image you did not create, and do not mount beamfs where
untrusted users can write to it.

Data loss:

- Sparse files: when the first block under a 4 KiB page is a hole, a
  read returns zeros for the whole page, including the data of the next
  block, and a later partial write of that page stores those zeros.
- Writes that start a new block inside a page that is not in the page
  cache can zero the rest of that page, data of the neighbouring blocks
  included (to be confirmed against the kernel's iomap).
- With two beamfs volumes mounted, the per-CPU cache of decoded
  indirect-parity regions can serve one volume's region to the other.
- `st_blocks` is always 0, so tools that trust it (`tar --sparse`)
  archive files as holes.
- `mkfs.beamfs --data-csum` on the default (interleaved) layout: every
  block rewritten by writeback or truncate, and every symlink target of
  96 bytes or more, fails its check on the next read (`-EIO`). The v3
  measurements did not use this option.

Security:

- A write does not clear the set-user-ID and set-group-ID bits.
- The link count is not bounded: 65536 hard links, which an
  unprivileged user can make, wrap it to zero on disk, and the inode is
  later freed while names still point to it.
- 16 bytes of uninitialised kernel memory are written into each data
  block on the default layout.

Kernel crashes:

- The superblock geometry is not validated: a crafted or corrupted
  image can crash the kernel at mount.
- A mount that beamfs refuses after reading the root inode (data
  protection scheme 5, unknown indirect-parity mode) crashes the kernel.

The measurements of the v3 report stand as measured: none of the tests
that ran detected these defects.
