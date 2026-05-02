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

## Critical path to kernel.org RFC

beamfs has not been submitted to linux-fsdevel. Distance to first
RFC mail measured against `Documentation/roadmap.md` Phase 1 to 8.

### Phase mapping (status per phase)

| Phase | Title                                  | Status      | Blocking items                |
|-------|----------------------------------------|-------------|-------------------------------|
| 1     | Format v5.0 minimal RFC-able           | not started | func-12                       |
| 2     | fsck.beamfs MVP                        | design only | func-6                        |
| 3     | Multiblock read_folio                  | active      | func-13 closed, substep 10 partial |
| 4     | Stage 4 close + paper v3               | not started | func-3, upstream-7            |
| 5     | DKMS + Yocto layer                     | not started | (no TODO item yet)            |
| 6     | Build user base (anti-NAK)             | not started | upstream-8                    |
| 7     | Documentation/filesystems + checkpatch | not started | upstream-3, upstream-4        |
| 8     | RFC mainline + review cycle            | not started | upstream-5, upstream-6, upstream-2 |

Each row maps an active TODO item to its phase. Items absent from
this table are quality improvements not on the RFC critical path
(see Tier 2/3 in Priority matrix at the bottom of this file).

See `Documentation/roadmap.md` for phase DoD definitions and effort
estimates. Total residual effort across phase 1-8 : ~400h.

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

## Upstream submission status

beamfs has not been submitted to linux-fsdevel. The previous "RFC v3
sent" status referred to the FTRFS lineage (Zenodo v1, v1.1, v1.2
published, project reached its limit). beamfs is a separate codebase,
re-pitched as RW + native RS-FEC per `Documentation/mainline-scope.md`,
and starts its mainline trajectory at Phase 1.

### upstream-1 : xfstests pass status unknown

**Status** : not run  
**Effort** : 1-2 weeks of real validation work (Phase 7 budget)  
**Repo** : beamfs

For linux-fsdevel mainline submission, reviewers will request "which
xfstests pass on beamfs". Currently this is not measured. Required
before mainline acceptance.

### upstream-2 : Reviewer feedback integration

**Status** : not yet engaged. beamfs RFC v0 has NOT been sent to
linux-fsdevel.  
**Effort** : variable, materialises after first beamfs RFC mail  
**Repo** : beamfs

Pre-requisites : upstream-1, upstream-3, upstream-4, upstream-5,
upstream-6, upstream-7, upstream-8, and Phase 1-7 DoD per roadmap.

### upstream-3 : checkpatch.pl --strict baseline

**Status** : `tools/checkpatch-precommit.sh` exists, no baseline run
archived. Current count of errors/warnings on .c/.h is unknown.  
**Effort** : 1-2 days clean-up after baseline run (variable per
finding count) (Phase 7 budget)  
**Repo** : beamfs

For Phase 7 DoD : 0 errors, 0 warnings, --strict, on every .c/.h
under root + new files added in Phase 1-6. Baseline run is
prerequisite to estimate the clean-up scope.

### upstream-4 : Documentation/filesystems/beamfs.rst

**Status** : absent. All current docs are .md under `Documentation/`
(project-internal format, not kernel canonical).  
**Effort** : 2-4 days (write + review for kernel doc style) (Phase 7
budget)  
**Repo** : beamfs

Mainline kernel filesystem docs follow restructuredText format and
live under `Documentation/filesystems/<fs>.rst`. Required for Phase 7
alongside checkpatch zero. Should mirror the structure of
`Documentation/filesystems/ext4.rst` :
- Overview + on-disk format pointer
- Mount options
- Feature flags inventory
- Userspace tools (mkfs.beamfs, fsck.beamfs)
- Limitations (cross-ref `Documentation/known-limitations.md`)
- References

Source content already exists in `Documentation/mainline-scope.md`,
`format-v5-design.md`, `threat-model.md`, `known-limitations.md`.
Phase 7 work = transformation .md -> .rst + adaptation to kernel doc
tone.

### upstream-5 : cover letter [PATCH RFC 0/N]

**Status** : no draft. No cover* file in repo.  
**Effort** : 1-2 days writing + iteration (Phase 8 budget)  
**Repo** : beamfs (Documentation/upstream/cover-letter-rfc-v0.md or similar)

Cover letter for first beamfs RFC submission to linux-fsdevel. Must
include :
- Position statement (RW + native RS-FEC, gap vs dm-fec RO)
- Threat model summary (cross-ref `threat-model.md`)
- Capabilities matrix (cross-ref `mainline-scope.md` section 3)
- Out-of-scope explicit list (RAID, snapshots, encryption...)
- Validation methodology (RadFI harness, cross-ref paper v3 DOI)
- User base summary (post Phase 6, cross-ref upstream-8)
- AI tooling disclosure per `Documentation/process/coding-assistants.rst`
  (this file does not exist yet either ; see Phase 7).

linux-fsdevel reviewers expect ~2-3 pages, dense, factual. Cover
should also explicitly position vs FTRFS lineage (which reached its
limit) to defuse possible reviewer pattern matching.

### upstream-6 : git send-email + linux-fsdevel subscription

**Status** : `git send-email` configuration for `aurelien@hackers.camp`
not validated. linux-fsdevel mailing list subscription status unknown.
yocto-docs subscription was required for the prior `96377f88c` patch
resend (cf userMemories) ; same pattern likely applies.  
**Effort** : 30 min config + subscription confirmation (Phase 8 budget)  
**Repo** : tooling, no commit needed

Pre-flight before any RFC mail :
- `git send-email --dry-run` must produce SMTP output without
  authentication failure (msmtp + Gmail App Password setup per
  userMemories nullmailer config).
- `subscribe linux-fsdevel <email>` to `majordomo@vger.kernel.org`
  acked.
- DKIM/SPF on hackers.camp checked (cover letter from this address
  must not be bounced or marked spam by vger.kernel.org).
- Test mail to self via `git send-email` validates the full chain.

### upstream-7 : paper v3 published on Zenodo with DOI

**Status** : `papers/2026-04-beamfs-v3-findings/SCIENTIFIC-FINDINGS-2026-04-30.md`
is a 16 KB raw findings note, not a finished paper. No LaTeX tree, no
PDF, no Zenodo DOI assigned.  
**Effort** : 100h per Phase 4 budget (writing + review + Zenodo
upload + DOI minting)  
**Repo** : beamfs (papers/2026-04-beamfs-v3-findings/)

Paper v3 is a Phase 4 deliverable. Once published, the Zenodo DOI is
cite-able in the RFC cover letter (upstream-5). Without a public paper
backing the resilience claim, the RFC narrative loses its empirical
anchor — same FTRFS pattern that led to NAK.

Workflow : LuaLaTeX + gnuplot per userMemories. Stack already
installed and validated on spartian-1.

Pre-requisites : Stage 4 close (func-3 data block protection), to not
publish a paper that becomes obsolete on Stage 4 merge.

### upstream-8 : user base 3-5 public deployments (anti-FTRFS-NAK)

**Status** : zero public beamfs deployments today.  
**Effort** : Phase 6 budget = 75h  
**Repo** : beamfs + external (deployment partners)

Phase 6 anti-FTRFS-NAK doctrine (cf `roadmap.md` Phase 6 + recadrage
acquis Phase 0) : reviewer pattern in linux-fsdevel for new
filesystems is "show me the users". FTRFS was rejected primarily on
this axis. beamfs RFC must cite >=3 public deployments at submission
time.

Possible deployment angles (to firm up as Phase 6 starts) :
- DKMS package on a Gentoo overlay or AUR (low barrier)
- Yocto layer dependency for an embedded distro
- One academic lab using beamfs+RadFI for radiation-tolerance research
- One specific industrial/commercial use case (OIV/HPC, ANSSI client...)
- Phoronix Test Suite integration (volume but anonymous)

Each deployment must be public, citable, and ideally backed by a
short testimonial or technical writeup. Quantity matters but quality
of citation matters more (Phoronix benchmark run alone is weaker than
one university lab using it in published research).

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

### beamfs-bench evolution: INLINE frontier scan methodology [NEW 2026-05-02]

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

### beamfs-bench evolution: worker attack/verify semantic redesign [CLOSED 2026-05-02]

**Status** : closed substep 10. Worker actions reworked from
random-overwrite to pristine-read under live RadFI attack ; verdict
derivation moved from `worker.sh` to `synthesis.rs` ; 9 cargo tests
cover the decision matrix.

Spec, decision tables, and observation record formats are documented
in `~/git/beamfs-bench/README.md` sections "Observation record formats"
and "Verdict derivation". Not duplicated here to avoid drift.

Cluster scope still emits factual records but does not yet apply the
`RS_RECOVERED|RS_PASSTHROUGH|...` derivation ; tracked as a follow-up.

### beamfs-bench evolution: host-side forensic capture + bpftrace opt-in [CLOSED 2026-05-02]

### beamfs-bench evolution: bpftrace VM-side via Yocto recipe [NEW 2026-05-02]

**Status** : not implemented. Required for VM-side BPF probes.
**Effort** : 1-2 days (recipe + kernel BTF/BPF features + runtime validation).
**Repo** : yocto-beamfs (recipes-kernel/bpftrace/) + IMAGE_INSTALL.

`bpftrace` is currently absent from the active Yocto layers (poky
styhead, meta-openembedded styhead). Host-side bpftrace is enabled
in `forensics_host.rs --bpftrace` ; VM-side requires :

  1. New local recipe `recipes-kernel/bpftrace/bpftrace_X.Y.bb`
     (sources upstream tarball, depends on libbpf/libelf/clang/llvm).
  2. Kernel config additions for BPF/BTF support in
     `recipes-kernel/linux/BEAMFS-arm64.cfg` :
       CONFIG_BPF_SYSCALL=y
       CONFIG_BPF_JIT=y
       CONFIG_DEBUG_INFO_BTF=y
       CONFIG_BPF_EVENTS=y
  3. Add bpftrace to `hpc-arm64-research-beamfs.bb` IMAGE_INSTALL.
  4. Validate runtime via `ssh compute01 sudo bpftrace -V` after
     image rebuild + VM redeploy.

When done, `forensics.rs::pre_capture_all` could optionally launch
guest-side bpftrace probes alongside the existing ftrace path under
`Scope::Full`.

**Status** : closed substep 10. New module `forensics_host.rs` (companion
to `forensics.rs` VM-side). Captures host context into `<run_dir>/host/`
so the existing `make_tarball` naturally embarks both VM and host
forensics in a single archive (no consolidation step).

Captured systematically (all scopes) :
  - `dmesg.log`, `uname.log`, `system.log` (free/uptime/cmdline)
  - `lsblk.log`, `lsusb.log`
  - `virsh-list.log`, `virsh-list-final.log`
  - `virsh-dumpxml-beamfs-{master,compute0[1,2,3]}.xml`
  - `canonical-ko.log` (sha256 + path of deployed .ext2)

Captured opt-in via `--bpftrace` flag :
  - `bpftrace.log` (block_rq_complete + sched_switch counts)
  - Requires NOPASSWD sudo on bpftrace ; gracefully skipped
    otherwise with a warning (no run abort).

bpftrace VM-side is out of scope : not in the Yocto image
`hpc-arm64-research-beamfs.bb`. Tracked as a follow-up if VM-side
probes are wanted later.

### beamfs-bench evolution: cluster_*/multifs worker duplication [NEW 2026-05-02]

**Status** : new finding from substep 9 review  
**Effort** : 4-6h refactor + tests  
**Repo** : beamfs-bench (worker.sh)

`worker.sh` defines `setup`/`attack`/`verify` (multifs scope, master
USB targets) and `cluster_setup`/`cluster_attack`/`cluster_verify`
(cluster scope, `/data` on each node). The two trios share ~80% of
the logic: same dir-A/B/C layout, same 9 random files, same
`find -type f -exec sha256sum`, same RadFI arm/disarm sequence, same
verdict ladder.

Differences are minimal: cluster uses `$SUBDIR=/data/beamfs-bench-$TS`
instead of `$MNT=/mnt/test-$FS`, cluster_verify lacks the `! -s
POST_FILE -> FS_PANIC` guard, and cluster_attack hard-codes `vdb`.

This duplication is the reason the worker attack/verify semantic
mismatch was discovered late: the same flaw lived twice. A common helper (`_setup_layout`, `_run_attack`,
`_compute_verdict`) that both paths call would have caught it.

### beamfs-bench evolution: mkfs.beamfs robustness + integration tests [NEW 2026-05-02]

**Status** : not implemented
**Effort** : 1-2 days
**Repo** : yocto-beamfs (mkfs-beamfs userspace tool) + beamfs-bench

**Motivation** : Multiple issues observed during substep 9 R19 forensic
investigation suggest that mkfs.beamfs and the resulting filesystem
state are not as robust as the kernel module:

1. Root directory baseline nlink count appears wrong (observed
   Links=9 then Links=10 across mkfs invocations on freshly zeroed
   /dev/vdb, expected Links=2). Possibly a stale VFS buffer cache
   issue from prior mounts that was never properly invalidated, but
   could also be a real bug in how mkfs initializes the root inode
   on-disk i_nlink field.

2. After mkfs, `mkdir <subdir>` returns "File exists" even on the
   freshly formatted filesystem, suggesting either (a) the directory
   block of the root inode is not properly zeroed at mkfs time and
   contains residual dirents from a previous filesystem, or (b)
   the kernel module sees stale buffer heads and reports phantom
   entries.

3. multifs scenario beamfs verdict regressed from RECOVERED 3/3
   (manifest 092901Z, pre-K) to FS_PANIC 3/3 (manifest 112736Z,
   post-K). Forensics show 0 dmesg WARN/BUG/Oops, just "read failed"
   from userspace `find ... | sha256sum` returning empty. Root cause
   is not yet isolated and may be:
   - Genuine regression introduced by commit K (tri-block fix)
     interacting with a mkfs-time edge case
   - Pre-existing mkfs bug masked in cluster scope (which uses a
     different /dev/vdb backing) but exposed in multifs scope
     (USB passthrough disk on compute01 vdg)

**Proposed scope** :

1. **mkfs.beamfs hardening** :
   - Add explicit memset(0) of root directory block(s) at mkfs time,
     before writing the `.` and `..` initial entries.
   - Verify on-disk root inode i_nlink is initialized to 2.
   - Add `--zero-data` flag (default on for safety) that zeros the
     full data area before writing the bitmap. Already idempotent
     in design but add explicit verification.
   - Add `mkfs.beamfs --check` post-format pass that re-reads the
     filesystem from disk and verifies all expected invariants
     (root inode mode, nlink, dirent count = 2 for `.` and `..`,
     superblock CRC, bitmap CRC, etc).

2. **beamfs-bench mkfs-roundtrip scope** :
   - Wipe device with dd zero
   - mkfs.beamfs
   - mount + sync + verify root nlink == 2, root dirents == [".", "..", "canary"]
   - Create N subdirs, M files per subdir, mix of sizes (1 INLINE,
     N INLINE direct, N INLINE indirect, tri-block aligned)
   - sync + drop_caches + umount + remount
   - Verify all entries readable, all sha256 match, root nlink correct
   - Repeat with different mkfs flags (-s inline, -s inode_universal,
     -N <inodes>, ...)

3. **Integration in R19 pipeline** :
   - Add as new phase between bootstrap and analyse
   - Failure must propagate to overall_rc to avoid silent regression
     like the one observed in 112736Z

**Defers / dependencies** : Should run BEFORE multifs phase in R19
to detect mkfs/baseline bugs early. Resolves the diagnosis-by-bisection
problem where multifs regressions could be caused by either kernel
or mkfs.

---

---

## radfi improvements

### radfi improvement: targeting precision (range/list/file-aware)

**Status** : not implemented. Identified empirically from R19 runs of
beamfs-bench substep 10.
**Effort** : 2-4 days (radfi API + beamfs FIEMAP support).
**Repo** : radfi (companion module) + beamfs (FIEMAP).

`target_block=0` is currently a broadcast mode (random flip across all
blocks of `target_dev` while RadFI is armed). Useful for Family A
stochastic SEU but insufficient for :

  - Family B adversarial bursts (need range or list of blocks)
  - File-precise targeting to prove RS-FEC functional correctness
    (need file -> block mapping)

Empirical evidence from R19 runs : multifs+cluster beamfs records
show RS_CORRECTED=0 even when FLIP_DELTA>0, because the broadcast
flips touch FS structural blocks (HASHES, dirent, metadata) more
often than RS-protected data blocks. The current bench mostly
observes RS_PASSTHROUGH instead of the desired RS_RECOVERED.

Two symmetric problems with one architectural fix :

  1. radfi side : add `target_block_range` or `target_block_list`
     debugfs entries (atomic write : start..end or comma-list).
  2. beamfs side : implement `.fiemap` in `inode_operations` so
     userspace `filefrag -b4096 <file>` returns the actual block
     list backing the file (already supported by ext4/btrfs/xfs).

beamfs-bench `worker.sh setup` already conditionally reads
`filefrag` for non-beamfs FS (line 153 : `[ "$FS" != "beamfs" ]`).
Once beamfs implements FIEMAP, the conditional is removed and the
TARGET_BLOCK becomes file-precise. radfi then accepts that block
value via the existing `target_block` debugfs entry.

For paper v3, this is the gap to either close or explicitly
document : the current bench measures FS resilience to broadcast
EM noise on the device, not file-level RS-FEC functional
correctness. Both are valid scientific questions, but the paper
narrative needs to match the measurement methodology.

### radfi improvement: mainline submission positioning

**Status** : not implemented. Forward-looking concern.
**Effort** : 2-4 weeks (RFC writing + reviewer cycle).
**Repo** : radfi (RFC patches) + Documentation/.

radfi is currently a companion out-of-tree module to beamfs (separate
Zenodo DOI 10.5281/zenodo.19885777). For lab use this is fine, but
two trajectories diverge if radfi is to live in production beyond
the bench :

  - Production deployments running beamfs do NOT need radfi loaded.
    radfi.ko stays optional, lab-only, distributed via the bench.
  - Mainline submission requires positioning radfi vs existing
    kernel fault injection facilities :
      - `fail_make_request` (block layer fault injection, existing)
      - `fault-injection` framework in lib/fault-inject.c (existing)
      - dm-flakey (DM target for transient I/O errors, existing)
    radfi distinguishes itself by simulating bit-level flips in
    flight on the bio path (not request rejection or whole-block
    corruption). This is the angle to defend in an RFC cover
    letter.

Decision needed before phase 8 (RFC mainline submission of beamfs) :
submit radfi as a separate RFC track, bundle it with beamfs as a
co-submission, or keep it permanently out-of-tree as a lab tool ?

## beamfs improvements

### beamfs improvement: on-disk format stability discipline

**Status** : architectural concern. No code change required if
discipline is held.
**Effort** : ongoing (review every format-touching commit).
**Repo** : beamfs (Documentation/format-v5-design.md).

Format on-disk has had several revisions during pre-mainline work :

  - v3 (legacy)
  - v4 (current, kvmalloc RS scratch buffers)
  - v4 strict (item 4b, format bump rejecting v3 at mount)
  - v5 design in progress (allocator scaling : bitmap chained ->
    block groups -> btree for 16 EB, 11 INCOMPAT bits, 4 RO_COMPAT
    bits, 3 COMPAT bits)

Tension between roadmap phase 6 (build user base, anti-FTRFS-NAK)
and ongoing format evolution :

  - phase 6 needs real users with real deployments
  - real users need a stable on-disk format (no painful migrations)
  - mainline RFC needs the format to be defendable

Resolution requires holding the discipline already documented in
`format-v5-design.md` : freeze v5 minimal at RFC submission time,
add features only via INCOMPAT/RO_COMPAT/COMPAT flags (ext4 pattern),
never break existing on-disk layouts. The discipline is documented ;
enforcement is per-commit reviewer responsibility.

No automation possible. This entry exists as a reminder for every
reviewer (including future Claude sessions) to check whether a
commit touches on-disk layout and refuse it if it breaks v5
minimal compatibility post-RFC submission.

### beamfs improvement: defense-in-depth silent skip observability

**Status** : implemented but needs ongoing surveillance.
**Effort** : ongoing (per-session forensic check).
**Repo** : beamfs (alloc.c).

The canary fix (substep 10, commit 3275202) added a 2-layer defense
against double-free of reserved blocks :

  - Layer 1 (inode.c) : `S_IMMUTABLE` flag set on reserved inodes
    at mount time. VFS rejects open/setattr/unlink etc. with EPERM.
  - Layer 2 (alloc.c) : `beamfs_free_block` and
    `beamfs_free_inode_num` silent skip + early return when called
    on reserved blocks/inodes (covers Layer 1 bypass via memory
    corruption mutating an `i_direct[]` in flight).

Layer 2 by design hides corrupted state : the call returns silently
instead of WARN+pr_err. This is correct in production (avoids panic
on suspected EM corruption) but masks real bugs in debug.

Mitigation already in place : `dump_stack()` after `pr_warn "double
free of block %llu"` in `beamfs_free_block` legitimate path. Zero
cost on success path (`test_bit` short-circuit), captures call chain
on error path.

Required surveillance per session :

  - Grep dmesg captures for `double free` + `dump_stack` markers
    after every R19. Investigate any non-zero count.
  - Do not rely on R19 exit=0 alone : Layer 2 silent skip can
    mask a real bug while the pipeline reports clean.
  - When a real bug is suspected : temporarily replace silent
    skip with `WARN_ONCE` or `pr_err+BUG_ON` for diagnosis, then
    restore silent skip.

## beamfs-bench improvements

### beamfs-bench improvement: cluster scope verdict derivation

**Status** : not implemented. Tracked as follow-up of substep 10
worker redesign.
**Effort** : 2-3h (port multifs derivation to cluster scope).
**Repo** : beamfs-bench (cluster.rs + synthesis logic).

`synthesis.rs` derives `RS_RECOVERED|RS_PASSTHROUGH|RS_FAILED|
FS_PANIC|CORRUPTED_DATA` for the multifs scope by cross-referencing
ATTACK and VERIFY records. The cluster scope emits factual records
in the same format (CLUSTER|HOST=...|HASH_PRE/POST/CAT_RC/
RS_CORRECTED/...) but no derivation runs on them. Cluster verdicts
remain raw (`VERIFIED|DIFFS=N`) at synthesis time.

Asymmetry to remove :
  - multifs : worker emits factual ; synthesis.rs derives logical
  - cluster : worker emits factual ; nothing derives logical

Action : write `derive_cluster_verdict()` mirroring
`derive_verdict_beamfs()` and apply per-node + per-prob.
Aggregate cluster verdict = worst case across 4 nodes
(FS_PANIC dominates, CORRUPTED_DATA next, RS_FAILED next,
RS_PASSTHROUGH next, RS_RECOVERED best).

### beamfs-bench improvement: radfi state timeline in forensics

**Status** : not implemented. Identified during forensics-1 review.
**Effort** : 1-2h (capture debugfs at transitions, ts annotation).
**Repo** : beamfs-bench (forensics.rs + worker.sh).

VM-side forensics capture dmesg, ftrace, perf, lsmod, but not the
`/sys/kernel/debug/radfi/{call_count,flip_count,target_dev,
target_block,probability,enabled,hook_blk}` content snapshots at
the arm/disarm boundaries.

Consequence : when investigating "did radfi flip the block beamfs
failed to recover ?", the answer requires reading dmesg for radfi
log entries (which exist but are not always emitted at every flip)
instead of reading a clean before/after counter snapshot.

Action : add `radfi-timeline.log` per-VM capture file. worker.sh
attack action already reads CALL_B/FLIP_B/CALL_A/FLIP_A for the
delta computation ; persist these per-attack with monotonic
timestamps. forensics post-capture appends final state. Output
format machine-parseable for cross-correlation with dmesg RS
corrected events.

### beamfs-bench improvement: factor multifs/cluster worker duplication

See `beamfs-bench evolution: cluster_*/multifs worker duplication`
above. Single source of truth ; no duplicate content here to avoid
drift.

## Priority matrix

Three tiers, ordered by impact on the kernel.org RFC critical path.

### Tier 1 - RFC critical path (Phases 1-8)

| Item        | Phase | Effort                | Note                                     |
|-------------|-------|-----------------------|------------------------------------------|
| func-12     | 1     | ~200 LOC + format bump| INLINE+INODE_RS feature flag             |
| func-6      | 2     | 3-5 days              | fsck.beamfs MVP                          |
| func-3      | 4     | 2-4 weeks             | Stage 4 data block protection            |
| upstream-7  | 4     | 100h                  | paper v3 Zenodo DOI                      |
| upstream-8  | 6     | 75h                   | user base 3-5 public deployments         |
| upstream-1  | 7     | 1-2 weeks             | xfstests pass status                     |
| upstream-3  | 7     | 1-2 days              | checkpatch baseline + cleanup            |
| upstream-4  | 7     | 2-4 days              | Documentation/filesystems/beamfs.rst     |
| upstream-5  | 8     | 1-2 days              | cover letter draft                       |
| upstream-6  | 8     | 30 min                | git send-email + subscription            |
| upstream-2  | 8     | variable              | reviewer feedback integration (post-mail)|

### Tier 2 - Quality (visible to reviewer, not strictly blocking)

| Item    | Effort  | Note                                          |
|---------|---------|-----------------------------------------------|
| gap-1   | 2-3h    | RAF acronym renaming                          |
| gap-3   | 1-2 min | kernel module redeploy                        |
| gap-4   | 0       | tarball audit-trail (informational)           |
| func-4  | 1.5h    | RadFI seed reproducibility                    |
| func-5  | 30 min+ | CRC32 mismatch scheme=2 diagnostic            |
| func-7  | 30 min  | Phase 09 post-forensics restructure           |
| func-10 | ~10 LOC | phys bound check in lookup_or_alloc_phys      |
| func-11 | ~30 LOC | pointer-corruption forensic counter           |

### Tier 3 - Nice-to-have (post-RFC or scope-extension)

| Item                                                | Effort | Note                          |
|-----------------------------------------------------|--------|-------------------------------|
| func-8                                              | 2-3 d  | multi-node FS-test            |
| func-9                                              | 7-9 d  | HPC stack activation          |
| All `beamfs-bench evolution:` items (open)          | varies | bench infrastructure          |
| All `beamfs-bench improvement:` items               | varies | bench infrastructure          |
| All `radfi improvement:` items                      | varies | companion module evolution    |
| All `beamfs improvement:` items                     | ongoing| discipline + observability    |

func-8 and func-9 are hardware-gated (15 USB homogeneous + hub budget).

### Cumulative effort to first RFC mail (Tier 1 only)

~400h, ~3-4 months sprint solo or ~8 months calendar at sustainable
pace. Critical path = upstream-7 (100h paper) + upstream-8 (75h user
base) + func-3 (2-4 weeks Stage 4) + func-6 (3-5 days fsck) ≈ 70% of
the budget.

---

## See also

- `Documentation/architecture-current.md` : current runtime architecture
- `Documentation/threat-model.md` : EM threat model + v4 journal nomenclature
- `Documentation/system-architecture.md` : positioning vs dm-verity, squashfs, VxWorks, PikeOS
- `Documentation/roadmap.md` : Stage 3 / Stage 4 / Stage 5 plan ; Phase 0-8 mainline preparation roadmap (DoD per phase)
- `Documentation/mainline-scope.md` : v5 position, threat model, capabilities matrix, out-of-scope
- `Documentation/format-v5-design.md` : v5 on-disk format design (skeleton, populated phase by phase)
- `Documentation/fsck.beamfs.md` : fsck.beamfs design (Phase 2 deliverable)
- `Documentation/known-limitations.md` : current scheme limitations
- `context/context-recadrage.md` : R0-R30 operational rules + Phase 0 strategic acquis (PRIVATE)
