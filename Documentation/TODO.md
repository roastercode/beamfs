# beamfs TODO list

> **CLASSIFICATION INTERNAL - NEVER PUSH TO PUBLIC GITHUB**
>
> This file is versioned only on `roastercode/beamfs-devel` PRIVATE
> branch `mainline-prep`. When `roastercode/beamfs` PUBLIC v3 is
> published (cf `context/context-recadrage.md` section 0), the
> `public-v3-staging` filter MUST exclude `Documentation/TODO.md`
> from the cherry-pick / rebase set. This file contains internal
> threat-model analysis, forensic findings, and pre-publication
> reasoning that is not public-facing.

**Authoritative**: this file is the single source of truth for outstanding
work across the three beamfs repositories. Each item carries an empirical
status, an effort estimate, and a cross-repo reference.

**Last updated**: 2026-05-02

**Cross-repository scope**:

| Repo                                    | Branch          | Visibility | Latest commit |
|-----------------------------------------|-----------------|------------|---------------|
| `roastercode/beamfs-devel`              | `mainline-prep` | PRIVATE    | `390676b`     |
| `roastercode/beamfs` (vitrine, frozen)  | `mainline-prep` | PUBLIC     | (frozen)      |
| `roastercode/yocto-beamfs`              | `main`          | PRIVATE    | `99cf71e`     |
| `roastercode/beamfs-bench`              | `main`          | PRIVATE    | `6c1e0c0`     |

---

## What is in good shape

This section is empirical: items here have been validated by mega run
20260501-225142 (10/10 phases PASSED, tarball SHA c1fd528b...) and by
the cross-repo lockstep R19 audit.

- mkfs.beamfs : operational, schemes 0..5 supported
- beamfs.ko kernel module : scheme 5 INODE_UNIVERSAL fully functional
- radfi.ko fault injection module : EM-style bit-flip injection working
- beamfsd userspace daemon : reads journal, signs peer protocol
- inject_raf, decode_raf_journal.py : tooling functional
- beamfs-bench (6 scopes : pipeline, multifs, analyse, bitrot, metadata, crash, fsck, mega)
- 4-VM aarch64 cluster (master + 3 computes) : isolation R21 enforced
- Lockstep R19 byte-identical 12 sources beamfs<->yocto-beamfs
- Build reproducibility : 12013 lines proc-config + 231 Yocto logs captured
- GPG-signed commits + R14 PRIVATE-only push workflow
- Naming canonical "beamfs - resilient filesystem" applied across docs/code
- v4 journal nomenclature "Electromagnetic Resilience Journal" applied
- Empirical paper-grade data : multifs, cluster (45 flips RECOVERED at 1M ppm),
  metadata 4 scenarios, crash 5 FS, fsck 3+1+1

---

## Documentation/code coherence gaps (4 items)

These are residual incoherences after the naming + EM rename passes.
None are blocking but they are visible to a reviewer.

### gap-1 : ABI tooling RAF acronym not yet renamed

**Status** : pending  
**Effort** : 2-3h  
**Repo** : yocto-beamfs (recipes-beamfs/beamfsd) + beamfs-bench (worker.sh consumers if any)

The descriptive nomenclature now says "Electromagnetic Resilience Journal"
in docs and recipe SUMMARY/DESCRIPTION. But the deployed tooling still
uses the legacy RAF acronym in identifiers visible at runtime:

- Binary `/usr/sbin/inject_raf`
- C function `scan_raf()` in beamfsd.c
- printf format `RAF[%02d] block=...` consumed by syslog parsers
- Network protocol log line `PEER %s RAF block=...`
- Python script `decode_raf_journal.py`

Renaming requires a coordinated toolchain pass: binary name + Makefile
target + recipe install path + syslog consumer scripts + Python script
filename + cross-references in beamfs-bench worker.sh if any.

### gap-3 : kernel module not yet redeployed on VMs

**Status** : pending bitbake rebuild + redeploy  
**Effort** : 1-2 min (bitbake setscene + scp)  
**Trigger** : commit `480b624` changed MODULE_DESCRIPTION in super.c

The 4 VMs currently run beamfs.ko built before the naming rename
(`modinfo` reports the old string `BEAMFS: Beam-Resilient Filesystem`).
Until rebuild + insmod cycle, runtime nomenclature does not match
source nomenclature.

The next mega run with a fresh bitbake will trigger this naturally.

### gap-4 : tarball mega 20260501-225142 contains pre-rename strings

**Status** : audit-trail-only (not blocking)  
**Effort** : 0 (informational)  
**Note** : Scientific data (45 flips RECOVERED, multifs verdicts, RS
FEC events) remains valid. Only the descriptive strings in modinfo.txt
and dmesg trace `BEAMFS Beam Electromagnetic File System, EM resilience`
reflect the pre-rename source state. A future mega run will produce a
post-rename tarball.

---

## Functional gaps (8 items)

These are missing features or known bugs in the beamfs/bench code.

### func-2 : Substep 6 truncate kernel patch [CLOSED 2026-05-02]

**Status** : closed by commit `b9d48f0` (beamfs-devel) + `8deee76`
(yocto-beamfs lockstep). `beamfs_inline_setattr` + helpers
(`free_blocks_from`, `zero_tail_block`) merged. Scope direct + single
indirect, ~2 MiB max file (allocator capacity in v4). Validated by
manifest `20260502T071854Z` (overall_rc=0). Tests 6 (truncate-down)
and 7 (truncate-up) + durability all PASS on master VM scheme=2.

### func-3 : Stage 4 data block protection

**Status** : not implemented (deliberately deferred)  
**Effort** : 2-4 weeks  
**Repo** : beamfs (file_inline.c, file.c, RS layer)

Empirical metadata/synthesis.md says explicitly: "BEAMFS scheme=5
(INODE_UNIVERSAL) protects metadata only; data block protection is
Stage 4 future work". Required for a complete EM resilience claim
on data + metadata.

### func-4 : RadFI seed reproducibility

**Status** : not implemented  
**Effort** : 1.5h  
**Repo** : beamfs-bench (cli arg) + radfi (debugfs param)

Empirically `radfi-counters.log` shows `seed=7448197064951793313` (random
boot-time). Mega runs are not bit-identical between executions. Critical
for paper bisect and for kernel reviewer reproducibility.

Fix : `--seed N` arg in beamfs-bench, propagated to radfi.ko via
`/sys/kernel/debug/radfi/seed`.

### func-5 : CRC32 mismatch scheme=2 diagnostic

**Status** : 3 events observed in mega run, 1.4% of flips  
**Effort** : 30 min diagnostic, fix variable  
**Repo** : beamfs (super.c CRC32 path)

Empirical compute01 dmesg analyse-scope:

beamfs: inode 13 CRC32 mismatch (no RS available, scheme=2)

Three events on 222 flips total. scheme=2 (CRC32 simple) cannot recover.
Either (a) those flips hit zones not protected by scheme=5 RS coverage
(documentation gap), or (b) scheme=2 fallback path is reachable when it
should not be in scheme=5 mode (logic bug).

Need to read super.c CRC32+RS paths to determine which.

### func-6 : fsck.beamfs not implemented

**Status** : not implemented (Phase 1.5 mainline-prep deferred)  
**Effort** : 3-5 days  
**Repo** : new tool, likely yocto-beamfs/recipes-beamfs/fsck-beamfs/

Empirical fsck/synthesis.md: `CHECK=NOT_IMPLEMENTED, fsck_beamfs_pending_phase_1_5`.
For upstream linux-fsdevel submission, reviewers will request offline
filesystem check. Userspace tool that parses SB + bitmaps + RS journal.

### func-7 : Phase 09 post-forensics restructure

**Status** : known empirical issue  
**Effort** : 30 min  
**Repo** : beamfs-bench (src/mega.rs phase 09 placement)

Empirical: forensics-beamfs-compute01/dmesg.log captured at Phase 09
contains only boot-time logs (timestamp 20.954 max). Reason: Phase 05
crash test does `virsh destroy compute01` which reboots the VM and
flushes dmesg. The post_forensics global thus has no real data.

Fix : either move post_forensics before Phase 05, or capture per-sub-scope
forensics after each sub-scope that mutates VM state.

### func-8 : Multi-node FS-test parallel scope

**Status** : not implemented (depends on hardware - func-9)  
**Effort** : 2-3 days when hardware ready  
**Repo** : beamfs-bench (refactor bitrot/metadata/crash/fsck::run)

Empirical : compute02/03 have call_count=23/22 in analyse-scope versus
417 on compute01. compute02/03 are under-utilized (cluster member but
not FS-test holder). Refactor needed when 15 USB layout is available.

### func-9 : HPC stack activation (Slurm + IOR + mdtest)

**Status** : recipes ready, not activated  
**Effort** : 7-9 days when hardware budget OK  
**Repo** : yocto-beamfs (recipes-hpc) + beamfs-bench (new hpc scope)

Slurm 25.11.4, munge 0.5.18, pmix 5.0.3 already in recipes. OpenMPI +
IOR + mdtest to add. New `beamfs-bench hpc` scope to submit IOR jobs
under RadFI. Blocked by hardware budget : 15 homogeneous USB + hub.
Architecture documented in this commit's companion file `architecture-current.md`.

---

## Upstream submission gaps

### upstream-1 : xfstests pass status unknown

**Status** : not run  
**Effort** : 1-2 weeks of real validation work  
**Repo** : beamfs

For linux-fsdevel mainline submission, reviewers will request "which
xfstests pass on beamfs". Currently this is not measured. Required
before mainline acceptance.

### upstream-2 : Reviewer feedback integration

**Status** : RFC v3 sent, feedback pending  
**Effort** : variable  
**Repo** : beamfs

Cover Message-ID `<20260414120726.5713-1-aurelien@hackers.camp>`.
Active reviewers : Matthew Wilcox, Darrick J. Wong, Andreas Dilger,
Pedro Falcato, Gao Xiang. Phoronix coverage. AI tooling disclosure
per `Documentation/process/coding-assistants.rst`.

---

### func-10 : phys bound check in lookup_or_alloc_phys [NEW 2026-05-02]

**Status** : not implemented  
**Effort** : ~10 LOC + canary  
**Origin** : runtime forensic 2026-05-02 R19 substep 7 manifest
20260502T073520Z (tarball `beamfs-bench-analyse-full-20260502-093118`).

**Observation** : when RadFI flips a bit in an `__le64 i_direct[i]`
or `i_indirect`, the resulting pointer value can be enormous
(observed: 0x0004000000000000, 0x0000100000000000,
0x000000080000001b). The current code passes this value to
`sb_bread()` which fails (logged as `sb_bread failed phys=N`),
then later the buffer cache prints `block N out of range`.

**Proposed fix** : in `beamfs_inline_lookup_or_alloc_phys` (and its
allocator-free read counterpart), add a bound check after reading
each `__le64` pointer:

```c
if (phys >= sbi->s_nblocks) {
    pr_err_ratelimited("beamfs/inline: ino=%lu iblock=%llu: bogus "
                       "phys=%llu (s_nblocks=%lu); pointer corrupted
",
                       inode->i_ino, b, phys, sbi->s_nblocks);
    return -EUCLEAN;
}
```

Defense in depth: turns a sb_bread failure into a structured
filesystem corruption signal earlier in the call chain.

**Defers / dependencies** : none. Independent of v5 work.

---

### func-11 : Pointer-corruption forensic counter [NEW 2026-05-02]

**Status** : not implemented  
**Effort** : ~30 LOC + RS journal entry type  
**Origin** : runtime forensic 2026-05-02 (tarball
`beamfs-bench-analyse-full-20260502-093118`).

**Observation** : pointer-corruption rejects (func-10) and inode
CRC32 mismatches (func-5) are visible only via `pr_err_ratelimited`
in dmesg. They are not counted, not journaled, and not exposed via
debugfs. Forensic post-attack reconstruction relies on dmesg ring
buffer survival.

**Proposed** : add per-counter in `beamfs_sb_info` (or extend the
RS event journal with a new `BEAMFS_RS_EVENT_FLAG_POINTER_CORRUPT`
type). Fields:
- `pointer_corrupt_count`  : monotonic counter
- `inode_crc_mismatch_count` : monotonic counter
- `sb_bread_failed_count`  : monotonic counter

Expose under `/sys/fs/beamfs/<dev>/forensics/` or via debugfs.

**Defers / dependencies** : independent. Synergistic with func-10.

---

### func-12 : Scheme INLINE+INODE_RS (v5 feature flag) [NEW 2026-05-02]

**Status** : design only, deferred to Phase 1 v5  
**Effort** : ~200 LOC + on-disk format bump  
**Origin** : runtime forensic 2026-05-02 - inode 6 CRC32 mismatch
observed 8 times on compute01 under RadFI saturation.

**Observation** : current schemes are mutually exclusive:
- scheme=2 UNIVERSAL_INLINE: data blocks RS-protected, inodes
  CRC32-detected only (no correction)
- scheme=5 INODE_UNIVERSAL: inodes RS-protected on metadata,
  data blocks via legacy iomap (no per-block RS)

Neither covers the full attack surface. RadFI flipping a bit in an
inode in scheme=2 produces irreversible CRC32 mismatch (no RS).

**Proposed** : v5 INCOMPAT feature flag
`BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS` (already reserved in
`Documentation/format-v5-design.md`) activates RS-protection on
inodes *in addition to* INLINE data blocks. Combined coverage:
- data blocks: RS(255,239) inline (existing scheme=2)
- inodes: RS encode/decode at write_inode/read_inode
  (existing scheme=5 path, ported to inline-aware code)

**Defers** : Phase 1 of mainline-prep roadmap (50h budget).
Cf `Documentation/format-v5-design.md` section 4.2 (profile flags).

---

### func-13 : INLINE multi-block tri-block folio coverage [CLOSED 2026-05-02]

**Status** : CLOSED 2026-05-02 (commit pending in this session)
**Effort actual** : 1.5h (diagnosis 1h + patch 30min)
**Repo** : beamfs (file_inline.c)
**Discovered** : 2026-05-02 substep 9 xfstests preparation

**Symptom** : Files between 17 and 524 INLINE blocks (~65 KB to ~2 MB)
became corrupted after umount/remount. Read returned EINVAL at offset
57344 (= folio index 14, the first tri-block folio). The frontier was
deterministic and reproducible.

**Root cause** : `beamfs_inline_folio_coverage` correctly computed
b_first/b_last for tri-block folios (when k_first > 2*INLINE_BYTES -
PAGE_SIZE = 3552, occurring at folio indices 14, 28, 42, ...). But the
code documentation and the loops in `beamfs_inline_read_folio` and
`beamfs_inline_writeback_folio` assumed b_last == b_first OR
b_first + 1 (bi-block max). The `else` branch in both loops applied
b_last semantics (slice_offset=0, slice_length=len_in_b_last) to ALL
b > b_first, including intermediate blocks in the tri-block case. This
caused intermediate blocks to be written/read with wrong slice
parameters, leading to silent on-disk corruption.

The bug was masked by:
- Substep 4-7 canary scope: file size 8000 bytes = 3 INLINE blocks
  (always bi-block max, never tri-block).
- Substep 8 mmap canary: 8000 bytes (same scope).
- Tests with file content not flushed: pagecache served reads correctly
  even when on-disk content was corrupted.

**Fix** : Three-way branch in read_folio + writeback_folio loops:
- b == b_first : slice [k_first, INLINE_BYTES) or [k_first, k_first+lbl)
  if b_last == b_first
- b == b_last : slice [0, len_in_b_last)
- b intermediate (tri-block case) : slice [0, INLINE_BYTES) full block

Plus correction of `lbl` calculation in `folio_coverage` for tri-block:
`lbl = fub - (INLINE_BYTES - k_first) - (b_last - b_first - 1) * INLINE_BYTES`.

Doc comments updated to reflect "1, 2, or 3 blocks" reality and to
document the tri-block periodicity (every 14 folios).

**Validation** :
- Frontier scan N=1..50: 18/18 OK after fix (was: N>=17 CORRUPTED).
- Large file scan 100 KB..1500 KB: all OK after fix.
- 2000 KB CORRUPTED with `iblock 524 beyond v1 indirect capacity` -
  this is the v1 format limit by design (12 direct + 512 indirect
  pointers = 524 blocks max ~ 2 MB), not a bug.
- 0 BUG/Oops/WARN in dmesg.
- checkpatch --strict: 0 errors / 0 warnings / 0 checks (90 lines).

**Defers / dependencies** : Closes the substep 9 xfstests blocker for
files in [3 INLINE blocks, 524 INLINE blocks] = ~12 KB to ~2 MB range.
Substep 9 xfstests subset can now proceed.

---

### bench-1 : beamfs-bench INLINE frontier scan methodology [NEW 2026-05-02]

**Status** : not implemented
**Effort** : 4-6h
**Repo** : beamfs-bench

**Motivation** : The tri-block bug (func-13) was discovered manually
during substep 9 preparation, not by automated regression testing. A
similar regression in future could go undetected if the test suite
does not exercise the full INLINE folio-coverage state space.

**Proposed scope** : Add a new beamfs-bench scope or sub-scope (e.g.
`beamfs-bench inline-frontier`) that systematically tests file sizes
across the INLINE block boundary regions:

1. **Frontier scan** : Test sizes N * BEAMFS_DATA_INLINE_BYTES for
   N in {1..50, 100, 200, 500} - covers tri-block periodicity (every
   14th folio) and the v1 indirect capacity boundary (~524 blocks).

2. **Read/write integrity** : For each size, write urandom content,
   sync, capture sha256 pre-umount, umount/remount, verify sha256
   matches. Report any divergence as CORRUPTED.

3. **Mmap sub-scope** : Same scan via mmap+msync paths to cover the
   address_space ops used by mmap-write workloads (substep 8).

4. **Truncate sub-scope** : Same sizes via ftruncate (extends + shrinks)
   to cover the setattr path (substep 6).

5. **Multi-file** : Mix of file sizes in same FS to exercise allocator
   and indirect block sharing-or-not patterns.

6. **Output format** : Compatible with manifest.json schema, signed
   GPG audit trail like other scopes.

**Defers / dependencies** : independent. Highly synergistic with
substep 9 xfstests (which exercises some but not all of these patterns
through generic test programs). Adding inline-frontier scope means
that any future kernel module change is regression-tested before
xfstests is even invoked, with deterministic boundaries (xfstests
generic tests don't deterministically hit folio-14 etc).

---

## Priority matrix (recommendation)

This is a recommendation, not a prescription.

| Tier            | Items                                  | Cumulative effort |
|-----------------|----------------------------------------|-------------------|
| Quick wins      | gap-1, gap-3, func-4, func-7           | ~4.5h             |
| Stage 3 closing | func-5 (partial diag DONE 2026-05-02)  | ~1 day            |
| Stage 4 prep    | func-3 (data block RS)                 | 2-4 weeks         |
| Upstream prep   | func-6 (fsck), upstream-1 (xfstests)   | 3-5 weeks         |
| HPC activation  | func-8, func-9                         | 7-9 days hardware-gated |

---

## See also

- `Documentation/architecture-current.md` : current runtime architecture
- `Documentation/threat-model.md` : EM threat model + v4 journal nomenclature
- `Documentation/system-architecture.md` : positioning vs dm-verity, squashfs, VxWorks, PikeOS
- `Documentation/roadmap.md` : Stage 3 / Stage 4 / Stage 5 plan
- `Documentation/known-limitations.md` : current scheme limitations
