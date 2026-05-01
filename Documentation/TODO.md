# beamfs TODO list

**Authoritative**: this file is the single source of truth for outstanding
work across the three beamfs repositories. Each item carries an empirical
status, an effort estimate, and a cross-repo reference.

**Last updated**: 2026-05-01

**Cross-repository scope**:

| Repo                                    | Branch          | Visibility | Latest commit |
|-----------------------------------------|-----------------|------------|---------------|
| `roastercode/beamfs-devel`              | `mainline-prep` | PRIVATE    | `04ab5bd`     |
| `roastercode/beamfs` (vitrine, frozen)  | `mainline-prep` | PUBLIC     | (frozen)      |
| `roastercode/yocto-beamfs`              | `main`          | PRIVATE    | `3a47e91`     |
| `roastercode/beamfs-bench`              | `main`          | PRIVATE    | `88e748e`     |

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

### func-2 : Substep 6 truncate kernel patch

**Status** : not implemented (Stage 3 closing item)  
**Effort** : ~1 day, ~40 LOC  
**Repo** : beamfs (super.c, file_inline.c, namei.c)

Hook `beamfs_inline_setattr` in `i_op` to handle truncate(). Currently
beamfs does not survive truncate() syscalls on inline-allocated files.

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

## Priority matrix (recommendation)

This is a recommendation, not a prescription.

| Tier            | Items                                  | Cumulative effort |
|-----------------|----------------------------------------|-------------------|
| Quick wins      | gap-1, gap-3, func-4, func-7           | ~4.5h             |
| Stage 3 closing | func-2 (substep 6 truncate), func-5    | ~1.5 day          |
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
