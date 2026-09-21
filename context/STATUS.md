# beamfs - Status flag

> **CLAUDE - READ FIRST**: open `context/00-MINDMAP.md` for project
> navigation, then `context/context-recadrage.md` (R0-R37) for the
> operational contract, then THIS file for current state.

Etat reel au 2026-05-02 morning, sub-steps 6 + 7 closed,
INLINE multi-block path runtime-validated on scheme=2.

Storage canonique : `~/git/beamfs/context/STATUS.md` (NEVER published).

---

## Acquis empirique (cumule, dont session 2026-05-01)

### Code
- **beamfs-bench full + 7 subcommands implementes** : multifs, analyse,
  full, bitrot (DONE 2026-05-01 morning), metadata (DONE 2026-05-01
  midday), crash + fsck (DONE 2026-05-01 afternoon).
- R-isolation enforce via `lifecycle.rs::assert_isolation_architecture()`
  Phase 0 pre-flight.
- 5 USB physiques migres beamfs-master -> beamfs-compute01 (rigueur
  R21).

### Yocto
- Image canonique `beamfs-research-image.bb` (R23).
- Recipe enrichi : strace, perf, trace-cmd, blktrace dans IMAGE_INSTALL.
- Image redeploy 4 VMs cluster.

### Bench resilience matrix
- 6 classes de tests x 5 FS = 77 observations factuelles par run.
- multifs RECOVERED 3/3 beamfs (probs 1k, 100k, 1M) reproduit.
- cluster 12/12 RECOVERED 4 nodes x 3 probs reproduit.
- crash 5/5 phases OK (ext4, ext3, btrfs, beamfs remount + journal
  replay).
- fsck 5/5 phases OK (ext4 fsck_rc=0, ext3 fsck_rc=0, btrfs no error,
  squashfs SKIP, beamfs NOT_IMPLEMENTED Phase 2).

---

## Commits + tags 2026-05-01 (chronologique)

### beamfs-devel PRIVATE mainline-prep
- `8dbdf21` docs(context): R-isolation + R-os-stack + bench rigour v3
- `73f52c1` docs(context): R23 R-image-canonique

### beamfs-bench PRIVATE origin/main
- `9c787f6` feat(bench): bitrot subcommand
- `b23ab2a` feat(bench): R-isolation Phase 0 pre-flight
- `7c390b6` docs(readme): R-isolation + bitrot DONE
- `3f54611` feat(bench): metadata subcommand (Test A)
- `df76a4d` feat(bench): crash + fsck subcommands (Test B + Test D)
- `d638949` docs(readme): mark bitrot/metadata/crash/fsck as DONE
- `97ca646` fix(forensics): wait on exact perf PID + validate header

### yocto-beamfs PRIVATE origin/main
- `50c339f` feat(image): blktrace IMAGE_INSTALL

### beamfs-overlay PRIVATE origin/main
- `011bf20` initial: Gentoo overlay (sys-fs/beamfs-bench live ebuild)

### radfi PRIVATE origin/main
(unchanged this session)

---

## R0/R16/R19 status

- R0  : cargo build --release : 0 warning, 0 error final
- R16 : 0 em-dash globally
- R19 : `beamfs-bench full --auto-confirm` exit 0 from `~/git/yocto-beamfs/`

R19 INVOCATION FROM CORRECT CWD : `cd ~/git/yocto-beamfs && beamfs-bench
full --auto-confirm`. The Rust binary searches yocto-beamfs repo root
from CWD (R24).

---

## Architecture cluster (R-isolation R21)

beamfs-master    .10  vda + vdb                        orchestrator
beamfs-compute01 .11  vda + vdb + vdc..vdg (5 USB)     FS-test holder
beamfs-compute02 .12  vda + vdb                        cluster compute
beamfs-compute03 .13  vda + vdb                        cluster compute


5 USB FS targets : ext4(vdc), ext3(vdd), btrfs(vde), squashfs(vdf), beamfs(vdg).

Network hpcnet virbr1 192.168.56.0/24.
SSH user hpcadmin, key ~/.ssh/hpclab_admin (no passphrase).

---

## Pending - prochaine session

(see `context/TODO.md` for the live deferred work list)

Top of mind :
1. Audit coherence finale documentation : DONE 2026-05-01 (handoff session)
2. R24-R28 ajout au recadrage : DONE 2026-05-01
3. **Pipeline MIL no-NAK operationnel** : DONE 2026-05-01 evening
   - beamfs-bench full = 9 phases fail-closed validation chain
   - manifest JSON GPG-signed audit-grade en Documentation/runs/
   - in-VM identity check (sha256 beamfs.ko match reference)
4. **Sub-step 4 INLINE-MULTIBLOCK CLOSED** : DONE 2026-05-01 evening
   - file_inline.c write_begin: -EFBIG removed, pos>>PAGE_SHIFT
   - readahead hygiene blank line fix (checkpatch 0 warnings)
   - pipeline R19 overall_rc=0, manifest 20260501T154348Z signed
   - in-VM beamfs.ko sha256 match on 4 nodes
   - multifs RECOVERED 3/3 + cluster RECOVERED 12/12
   - dmesg clean on 4 nodes
   - commit beamfs cd02a547 (devel/mainline-prep PRIVATE)
   - commit yocto-beamfs b5e277e (origin/main PRIVATE) validate
   - commit beamfs-bench 9cd6e50 (origin/main PRIVATE) pipeline
5. **Recadrage section 0 cristallise** : DONE 2026-05-01 evening
   - mapping v2 PUBLIC / devel v3 PRIVATE / v3 PUBLIC futur
   - conditions publication v3 explicitees
   - lecture ligne-a-ligne obligatoire renforcee
6. **Sub-step 5 INLINE-MULTIBLOCK** : NEXT (writepages multi-folio +
   RMW writeback + per-inode allocation mutex). Effort estime 3h.
   Design : context/INLINE-MULTIBLOCK-DESIGN.md section 2.4 + 5
   step 5. C'est le sub-step qui exerce reellement write_begin
   multi-block au runtime (cross-block-boundary writes).
7. Stage 3 metadata hardening : ACTIVE (cf roadmap.md)
8. fsck.beamfs MVP : Phase 2 mainline-prep, future session

End of STATUS.md.
