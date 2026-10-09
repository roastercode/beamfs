# beamfs - resilient filesystem

beamfs is an EM-resilient Linux filesystem. Every data block is a capsule
of sixteen interleaved Reed-Solomon RS(255,239) codewords; every read
decodes it, and a read beamfs cannot vouch for fails with `-EIO` instead
of returning wrong bytes. It descends from FTRFS and targets mainline
Linux.

## Changelog

**Warning:** beamfs up to 0.1.26 has known defects that lose data,
weaken security or crash the kernel; read the 0.1.26 entry below before
using it.

The full record is in [CHANGELOG.md](CHANGELOG.md); its latest entries
follow.

### [Unreleased]

#### Changed

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

#### Added

- This changelog. The README carries its latest entries at the top, and
  the warning on the known defects of 0.1.26 is now one of them.

#### Fixed

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

### [0.1.26] - 2026-10-02

The version the v3 report measures (commit `1bf151d`).

#### Known defects

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

## State: beamfs v3

The beamfs v3 technical report measures beamfs 0.1.26, commit `1bf151d`,
on Linux 7.3-rc5, on x86-64 and aarch64:

- xfstests, `auto` group, 734 tests: all 122 that ran on x86-64 passed,
  and 122 of the 123 that ran on aarch64 (`generic/476` was stopped by
  its time budget). xfstests declined the others, most of them for
  features beamfs does not implement; they are counted apart, not as
  passes.
- Single-bit upsets injected into read bios by emufi 0.8.1, on a
  four-node aarch64 cluster and on USB flash media: beamfs never
  returned wrong data. It returned the correct file in every exercised
  case but one, and in that one refused the read. In the same runs ext4
  and ext3 returned silently corrupted data, and btrfs refused the read.
- The injector itself is audited: on this kernel emufi logs one of its
  two read hooks one bio before the block it flips, and the report
  reconstructs the hook of every flip.

The limits of these measurements are in section X of the report, the
work toward v4 in section XI.

- Report: [10.5281/zenodo.23253350](https://doi.org/10.5281/zenodo.23253350)
  (all versions: [10.5281/zenodo.19886191](https://doi.org/10.5281/zenodo.19886191)).
  The record also holds the workspace with the raw run records, the
  root images and kernels measured, their build records, and the source
  trees of the Yocto layer, the xfstests harness and the bench at the
  commits used.
- Sources of the report: [`papers/2026-10-beamfs-v3/`](papers/2026-10-beamfs-v3/).
- Tags: `beamfs-v3` is the measured code plus the report; `beamfs-v2` is
  the public `main` before v3.

## Publications

- beamfs v3, technical report: [10.5281/zenodo.23253350](https://doi.org/10.5281/zenodo.23253350)
- beamfs v2: [10.5281/zenodo.19886192](https://doi.org/10.5281/zenodo.19886192)
- EMUFI v1, the fault injector: [10.5281/zenodo.20041762](https://doi.org/10.5281/zenodo.20041762)
- RadFI v1, methodology: [10.5281/zenodo.19885777](https://doi.org/10.5281/zenodo.19885777)
- FTRFS v1, the predecessor: [10.5281/zenodo.19824442](https://doi.org/10.5281/zenodo.19824442)
- FTRFS RFC to linux-fsdevel, April 2026: [lore.kernel.org](https://lore.kernel.org/linux-fsdevel/20260414120726.5713-1-aurelien@hackers.camp/T/)
- ORCID: [0009-0002-0912-9487](https://orcid.org/0009-0002-0912-9487)

## Lineage

The FTRFS name and original concept originate in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: <https://doi.org/10.1007/978-3-319-16086-3_8>

FTRFS v1 (Desbrieres, 2026) is an independent open-source realisation of
that design on contemporary Linux. beamfs is a new filesystem with its
own on-disk format rather than a further FTRFS revision: the limits of
FTRFS are conceptual (threat model, where correction sits, scale, kernel
integration), as section II of the v3 report sets out. The soundness
theorem of the first beamfs report was falsified by fault injection and
withdrawn in v2.

## Repository layout

- `*.c`, `*.h`: kernel module sources
- `Kconfig`, `Makefile`: kernel build glue
- `tools/`: userspace tools (`mkfs.beamfs`, `fsck.beamfs`) and helper scripts
- `Documentation/`: design notes, on-disk format, known limitations, roadmap
- `papers/`: LaTeX sources of the reports
- `context/`: working rules and notes of the project

## Build

beamfs is built into the kernel of a Yocto image by the layer
`yocto-beamfs`, which carries a copy of the module sources; for v3 every
source file of the module was checked to have the same hash in this
repository and in the layer. The v3 measurements used layer commit
`9d43172`, Linux 7.3-rc5 (`72d3fcf8`) and `CONFIG_BEAMFS_FS=y` on both
architectures. The layer is not public: its
source tree at `9d43172` is in the Zenodo record of the v3 report, with
the images built from it.

A build of the module alone against a kernel tree:

    make KDIR=<path-to-kernel-build>

checks that it compiles; only the image build exercises the target
kernel.

## Companion repositories

- [beamfs-xfstests](https://github.com/roastercode/beamfs-xfstests): the
  xfstests harness used for v3.
- [beamfs-bench](https://github.com/roastercode/beamfs-bench): the bench
  that ran the fault-injection campaign of v3.
- `yocto-beamfs` (the Yocto layer, which also carries emufi 0.8.1) and
  `beamfs-overlay` (Gentoo ebuilds) are not public; their source trees
  at the commits the v3 measurements used are in the Zenodo record.

## Status

- [x] beamfs 0.1.26 measured on two architectures (v3 report)
- [x] Code public on GitHub, reports on Zenodo
- [ ] v4, the items of section XI of the v3 report: indirect blocks with
      a whole parity slot and a generation, `DATA_CSUM` and `DATA_SELFID`
      by default, parity interleaving, a fixed injector, a beam campaign
- [ ] beamfs 0.2.0: the fixes of the known defects of 0.1.26 (see the
      changelog), each with its test
- [ ] beamfs RFC to linux-fsdevel

## Cite

See [`papers/2026-10-beamfs-v3/README.md`](papers/2026-10-beamfs-v3/README.md).

## License

GPL-2.0-only (kernel module + userspace tools), CC-BY-4.0 (papers).
See `COPYING` and paper headers.
