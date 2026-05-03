# checkpatch baseline run -- 2026-05-03

Sub-1.F livrable 1: baseline checkpatch run on the v5 kernel sources
of beamfs, archived as anti-regression reference for Phase 7
(checkpatch --strict zero DoD).

## Metadata

- Date: 2026-05-03
- Branch: diag/double-free-block
- Beamfs commit: 79d41da805be3e66182841d8df7a12db194385b4
- Kernel target: 7.0.3 (linux-mainline, Yocto build-qemu-arm64)
- checkpatch.pl path: ~/yocto/poky/build-qemu-arm64/tmp/work-shared/qemuarm64/kernel-source/scripts/checkpatch.pl
- checkpatch.pl sha256: 6e7abb5d613b8916f162db09d4442fbd444f31d2402022b641e9ecfcc3279323
- Mode: --no-tree --strict --file

## Totals

- errors:   2
- warnings: 23
- checks:   91
- lines checked: 4867

See summary.txt for the per-file breakdown.

## Files scanned

alloc.c, dir.c, edac.c, file.c, file_inline.c, inode.c, namei.c,
super.c, beamfs.h (9 files, 4867 SLOC by wc -l).

## Reproduction

    CKP=~/yocto/poky/build-qemu-arm64/tmp/work-shared/qemuarm64/kernel-source/scripts/checkpatch.pl
    cd ~/git/beamfs
    perl $CKP --no-tree --strict --file alloc.c dir.c edac.c file.c \
                                      file_inline.c inode.c namei.c \
                                      super.c beamfs.h

## Per-file reports

See <file>.txt in this directory. Each file's full checkpatch output
is preserved verbatim.

## Phase 7 trajectory

The Phase 7 DoD is checkpatch --strict zero ERROR/WARNING/CHECK.
This baseline records the current delta. Subsequent commits should
reduce the counts.

The run is intentionally --no-tree: beamfs is out-of-tree until
Phase 8 RFC submission. After in-tree migration (fs/beamfs/), the
baseline should be re-captured with --tree=<linux-src>.
