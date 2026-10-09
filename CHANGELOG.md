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

### Changed

- Comments rewritten to describe the code as it is: the scrubber writes
  a corrected block back, the block bitmap is on disk, a capsule
  survives a burst of 129 coded bytes (not 139) and its parity is not
  interleaved, RS-mode indirect parity reports damage without repairing
  it, readdir positions are byte offsets, the size limits are those of
  the code. References to documents outside the tree, the project's
  stage numbering, version history and validation claims nothing in the
  tree substantiates are removed. Only comments and white space change:
  with the comments stripped, every source file is the same token
  stream.
- `u8` and `u16` rather than `uint8_t` and `uint16_t`, `!p` rather than
  `p == NULL`, and `le16_add_cpu`, as checkpatch --strict asks; no
  change in behaviour.
- `refcount_t` rather than `atomic_t` for the slices of a folio still
  under writeback, as coccicheck asks: a count that would go below zero
  warns instead of wrapping.
- The scrubber takes its snapshot of a block in a function of its own,
  where the allocation mutex is taken and released unconditionally, and
  a brace block that ran to the end of `beamfs_scrub_one_inode` is gone;
  no change in behaviour.
- A test that could never be true (an 8-bit name length compared with
  255) becomes a build-time check, `max()` replaces a spelled-out
  maximum, and two lines past 100 columns are reflowed; no change in
  behaviour.

### Added

- This changelog. The README carries its latest entries at the top, and
  the warning on the known defects of 0.1.26 is now one of them.

### Fixed

- The module links on i386: 64-bit divisions go through `div_u64`,
  `div_u64_rem` and `DIV_ROUND_UP_ULL` (before, `__udivdi3` and
  `__divdi3` were left undefined).
- With `CONFIG_BEAMFS_ORDERED_META=y`, a newly allocated data block was
  written and waited on after its buffer had been released, in four
  places; the buffer is now released afterwards. Without that option
  nothing changes.
- A mount that runs out of memory while setting up the scratch pool,
  the superblock RS staging or the writeback page pool no longer leaks
  the superblock buffer, its copy and the pending RS events.
- When the per-CPU scratch array cannot be allocated at module load, a
  case meant to be survivable, the codec setup no longer writes through
  a NULL per-CPU pointer; each scratch allocation is tested, and the
  CPUs left without one are reported once.
- The module load fails with `-ENOMEM` when the Reed-Solomon codec
  cannot be set up, instead of registering a filesystem whose every
  encode and decode returns `-EINVAL`; a load that fails after the
  codec is set up releases it.

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
