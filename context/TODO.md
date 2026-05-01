# beamfs - TODO list (Claude-aware deferred work)

> **CLASSIFICATION INTERNAL - NEVER PUSH TO PUBLIC GITHUB**
>
> Linked from `context/STATUS.md`. Versioned only on
> `roastercode/beamfs-devel` (private), via `.gitignore` exception.
>
> **CLAUDE - READ FIRST**: see `context/context-recadrage.md`
> for the operational contract before touching anything in this list.

---

## TODO 1 - Lowercase normalization (deferred, manual review required)

**Why deferred** : 350 occurrences of `beamfs` in `.md`/`.txt` and
~950 in `.c`/`.h` strings/comments across `beamfs` and `yocto-beamfs`.
The kernel.org convention is lowercase `beamfs` in prose and dmesg
strings (cf. `btrfs`, `ext4`, `squashfs`). However, automated
substitution would damage the following non-trivial cases:

### 1.1 - Cases that must NEVER be lowercased

- Macros: `BEAMFS_MAGIC`, `BEAMFS_BLOCK_SIZE`, `BEAMFS_DATA_*`,
  `BEAMFS_VERSION_*`, `BEAMFS_FORMAT_V4`, `BEAMFS_RS_*`,
  `BEAMFS_FEAT_*`, `BEAMFS_SB_RS_*`, `BEAMFS_INODE_*`,
  `BEAMFS_INDIRECT_PTRS`, `BEAMFS_SUBBLOCK_*` - all C identifiers
  in ALL_CAPS (kernel coding style).
- Magic literal: `"BEAM"` in `0x4245414D` is a 4-byte ASCII magic,
  not a name. Never lowercase.
- All content under `papers/` (`.tex`, `.bib`, `.aux`, build artefacts)
  - formal scientific publication, published on Zenodo with fixed DOI.
  No textual modifications.
- Tags and commit messages already pushed (signed history).
- File `Documentation/format-v4.md` is the on-disk format
  specification, normative, referenced by the macros it documents.
  Prose like "beamfs v4 is the on-disk format" must stay aligned
  with `BEAMFS_FORMAT_V4` macro.
- File `Documentation/EMPIRICAL-FINDINGS.md` (title `# beamfs v1 ...`):
  dated session document, historical reference, do not retitle.

### 1.2 - Cases to lowercase, by hand, file-by-file

| File | Action | Risk |
|---|---|---|
| `README.md` (beamfs public) | review only - this is the public face, casing is part of branding | high - discuss before |
| `Documentation/2026-04-28-validation-session.md` | lowercase prose mid-sentence, keep `FTRFS↔beamfs` sigle in tables | low |
| `Documentation/design.md` (titles excluded) | lowercase prose body | medium - many macro mentions |
| `Documentation/multifs-bench-2026-04-30.md` | lowercase the `\| beamfs \|` table entries to align with `\| ext4 \|` | low |
| `Documentation/system-architecture.md` | lowercase prose body | low |
| `Documentation/testing.md` | lowercase prose | low |
| `Documentation/threat-model.md` | lowercase prose, keep formal references uppercase | medium |
| `Documentation/roadmap.md` | lowercase prose | low |
| `Documentation/known-limitations.md` | lowercase prose | low |
| `.c`/`.h` SPDX-like headers (`* beamfs - XXX`) | lowercase to `* beamfs - XXX` per mainline (`* btrfs ...`) | low |
| `.c`/`.h` `pr_*` log strings: `"beamfs: ..."` prefix is ALREADY lowercase. Only the descriptive text after the prefix needs review (e.g. `"beamfs Beam Electromagnetic"` → `"beamfs"` only) | low |

### 1.3 - Concrete examples to handle manually

super.c:	pr_info("beamfs: module loaded (beamfs Beam Electromagnetic File System, EM resilience)
");
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
-> "beamfs (Beam Electromagnetic File System, EM resilience)"


### 1.4 - Recommended approach when bandwidth allows

1. One commit per file (auditable, revertable).
2. `git diff` review before each commit.
3. Skip `papers/`, all `BEAMFS_*` macros, `"BEAM"` magic literal,
   `EMPIRICAL-FINDINGS.md` title, `format-v4.md` normative refs.
4. Test build (`bitbake beamfs-module`) after each `.c`/`.h` commit.
5. Run `beamfs-bench analyse --scope=full` (ex-`Tir-analyse-multifs.sh`) after the kernel-source pass to confirm
   no regression.

---

## TODO 2 - beamfs-bench (Rust unified harness) - DONE 2026-05-01

**Status (2026-05-01)** : DONE. The unified Rust harness `beamfs-bench`
is operational and packaged via Gentoo overlay `sys-fs/beamfs-bench`.

Ancestor scripts -> beamfs-bench mapping :

  - Tir.sh (443 lines)              -> covered by `beamfs-bench full`
  - Tir-analyse.sh (206)            -> `beamfs-bench analyse` (DONE)
  - Tir-analyse-rapide.sh (27)      -> `analyse --scope=quick` (DONE)
  - Tir-multifs.sh (425)            -> `beamfs-bench multifs` (DONE)
  - Tir-analyse-multifs.sh (75)     -> `analyse --scope=full` (DONE)
  - hpc-benchmark-beamfs.sh         -> covered by `beamfs-bench full` (DONE)

The new `beamfs-bench full` subcommand (added 2026-05-01) chains:
lifecycle (VM destroy/start/wait_ssh) + bootstrap (mkfs.beamfs + mount
/data on 4 nodes) + analyse scope=full (multifs + cluster + forensics).
This is the canonical pre-push validation per R19.

Total: 1176 lines of bash assembled into a single Rust binary
`beamfs-bench` with subcommands. Validation matrix executed against
the live 4-node beamfs cluster on 2026-04-30 (multifs, analyse quick,
analyse standard, analyse full); 5 successful runs covering 5 FS,
3 probabilities, and cluster-wide RadFI saturation on 4 nodes
simultaneously (48 confirmed flips, 0 corruption).

**Remaining work to fully close TODO 2** :

1. DONE (2026-04-30) : `beamfs-bench` extracted to private repo
   `roastercode/beamfs-bench` via subtree split.

2. DONE (2026-05-01) : legacy `bin/Tir-*.sh` scripts in yocto-beamfs
   superseded by `beamfs-bench full`. Files kept in place for
   archivistic reasons (cited in 2026-04-30 papers/forensics) but
   no longer invoked. R19 forbids running them.

3. DONE (2026-05-01) : `Tir.sh` HPC iobench coverage absorbed by
   `beamfs-bench full` Phase 6 (cluster_setup + cluster_attack +
   cluster_verify on 4 nodes). The dedicated `beamfs-bench bench`
   subcommand is no longer needed; cluster perf measurement happens
   inside `analyse --scope=full` cluster phase.

4. DONE 2026-05-01 : 4 new scopes implemented and validated:
   `bitrot`   (Test C - dd random on offline partition)            DONE morning
   `metadata` (Test A - superblock/inode/journal targeted attack)  DONE midday
   `crash`    (Test B - virsh destroy mid-write + remount)         DONE afternoon
   `fsck`     (Test D - fsck recovery post-FS_PANIC)               DONE afternoon
   All 4 modules : 5/5 to 20/20 phases OK, exit 0, R19 vert.

5. DONE 2026-05-01 : `forensics::stop_perf_master` perf header
   data_size=0 race fixed in beamfs-bench `97ca646`. start_perf_master
   now writes /tmp/beamfs-bench-perf.pid via `bash -c '... & echo $!'`,
   stop_perf_master SIGINTs that exact PID, bounded-waits via `kill -0`
   for process exit, then validates the data file with `perf report
   --header-only`. Marker on success: `header_ok=1`. Empirical
   validation (analyse --scope=full): perf_stopped_size=44212536
   header_ok=1, 237K samples readable, no data_size=0 error.

6. DONE 2026-05-01 : Gentoo overlay pushed to PRIVATE
   `roastercode/beamfs-overlay`. Initial commit `011bf20` GPG-signed,
   layout standards-compliant (masters=gentoo, thin-manifests).
   Initial package: sys-fs/beamfs-bench (live ebuild 9999, cargo +
   git-r3, EGIT_REPO_URI=file:///home/aurelien/git/beamfs-bench).
   Sudoers entry /etc/sudoers.d/beamfs-bench dropped via
   insinto/doins in src_install (NOPASSWD virsh for group libvirt).
   Visibility verified PRIVATE post-push (gh repo view).

### 2.1 - Subcommand layout (final, 2026-05-01)

beamfs-bench version       # DONE
beamfs-bench multifs       # DONE - 5 FS x 3 probs head-to-head on USB
beamfs-bench analyse       # DONE - forensic wrapper (3 scopes: quick/standard/full)
beamfs-bench full          # DONE - lifecycle + bootstrap + analyse scope=full
                           #        canonical pre-push validation (R19)
beamfs-bench metadata      # DONE - Test A - metadata-targeted attack
beamfs-bench crash         # DONE - Test B - crash consistency
beamfs-bench bitrot        # DONE - Test C - bit-rot offline
beamfs-bench fsck          # DONE - Test D - fsck recovery post-FS_PANIC


Common flags (existing) : `--auto-confirm`, `--dry-run`, `--scope`,
`--no-tarball`, `--shutdown`, `--skip-vm-bootstrap`.

### 2.2 - Crate dependencies (proposed)

- `clap` v4 - argument parsing
- `serde` + `serde_json` - JSON output for synthesis
- `ssh2` or `russh` - SSH execution to master VM
- `chrono` - timestamps
- `anyhow` + `thiserror` - error handling
- `tracing` - structured logging

### 2.3 - Estimate

- Skeleton + Cargo.toml + multifs subcommand: 1h
- Port Tir-multifs.sh → multifs.rs: 2h
- Port Tir-analyse-multifs.sh wrapping → analyse.rs: 1h
- Tests A/B/C/D: 3-4h
- Forensic capture (dmesg/ftrace/perf): 1-2h
- Validation runs: 2h

Total: 10-12h focused work.

### 2.4 - Naming policy (anti-NAK)

- Binary name: `beamfs-bench` (lowercase, hyphenated, mainline-style)
- Crate name: `beamfs-bench`
- Subcommands: lowercase verbs/nouns
- Log output: `beamfs-bench: ...` prefix (lowercase, like dmesg)
- Module structure: `src/multifs.rs`, `src/metadata.rs`, etc.

### 2.5 - Migration plan (executed 2026-04-30 to 2026-05-01)

1. DONE 2026-04-30 : `beamfs-bench multifs` ported, verdicts byte-identical
   to reference run beamfs-bench-analyse-20260430-141008 (renamed 2026-05-01).
2. DONE 2026-04-30 : `beamfs-bench analyse` ported (3 scopes).
3. DONE 2026-05-01 : `beamfs-bench full` added (lifecycle + bootstrap +
   analyse scope=full). Replaces the originally-planned `bench` subcommand
   which was redundant with `full`'s cluster phase.
4. DONE 2026-05-01 : 4 new test scopes (metadata, crash, bitrot, fsck).
5. PENDING : decide whether to physically remove `bin/Tir-*.sh` (currently
   kept in place, no longer invoked per R19).
6. PENDING : bench-v2 multi-capability report
   (`Documentation/multifs-bench-v2-*.md`) once 4 new scopes land.

---

## TODO 3 - INLINE-MULTIBLOCK sub-steps (Sub-step 4 DONE 2026-05-01)

Continue the 10-step INLINE-MULTIBLOCK-DESIGN.md sequence:

- Sub-step 4: `write_begin` multi-block (remove `-EFBIG`), 30 min - DONE 2026-05-01 (commit beamfs cd02a547, manifest 20260501T154348Z, R19 overall_rc=0)
- Sub-step 5: `writepages` multi-folio + `writeback_folio` +
  scratch kmalloc + `i_alloc_mutex`, 3h
- Sub-step 6: `setattr` truncate, 1h
- Sub-step 7: sparse + durability tests, 30 min
- Sub-step 8: `.mmap = generic_file_mmap`, 30 min
- Sub-step 9: xfstests subset (generic/001, 002, 010, 098, 257), 2-4h
- Sub-step 10: checkpatch strict zero CHECKs, 1h

Total remaining for full multiblock: 8-10h focused work.

---

## TODO 4 - Yocto image cleanup

`yocto-beamfs/Documentation/runs/` accumulates Tir-* runs. Currently
~17 untracked directories + 5 tarballs. Decide a retention policy:

- Keep only the 3 latest validated runs (multifs-141008, multifs-193032,
  + future).
- Older runs: tarball + remove.
- Add `.gitignore` for `Documentation/runs/Tir-*` except a
  curated set.

Estimated 30 min.

---

## TODO 5 - `.bb` SUMMARY/DESCRIPTION yocto

Yocto `.bb` recipes use `beamfs` in SUMMARY/DESCRIPTION fields.
Yocto convention: first letter capitalized, rest lowercase
("Btrfs userspace tools" not "BTRFS userspace tools").

Files to review (~34 occurrences):
- `recipes-kernel/beamfs/beamfs-module_0.1.0.bb`
- `recipes-kernel/beamfs/mkfs-beamfs_0.1.0.bb`
- `recipes-kernel/beamfs/beamfsd_0.1.0.bb` (if exists)
- `recipes-core/images/hpc-arm64-research-beamfs.bb`

Estimated 15 min, low risk.

---

## TODO 6 - Update STATUS.md to reflect current acquis (post-substep-2-3)

Current STATUS.md is dated `2026-04-30 fin-session migration labo` and
predates sub-steps 2-3 multiblock-read. It needs an update to mention:

- `v1.5.0-rc1-substeps2-3-multiblock-read` (beamfs devel)
- `v1.5.1-context-recadrage-v1` (beamfs devel)
- Capabilities matrix section in multifs-bench report
- TODO.md (this file) as the live deferred work list

Estimated 10 min.

---

## TODO 7 - Pipeline MIL no-NAK improvements (low priority)

The pipeline `beamfs-bench full` is operational since 2026-05-01
with 9 phases fail-closed and GPG-signed audit manifest. Possible
improvements identified during the substep-4 validation session
(none blocking, all incremental hardening) :

### 7.1 - Manifest provenance enrichment

Add to manifest JSON :
- ext2 deployed sha256 per VM (4 entries) -- proves redeploy
  reached the libvirt images dir, not just that the build artifact
  was correct.
- super.c warnings count (frame-larger-than) -- track regression.
- bitbake task summary count (Attempted vs Succeeded).
- gpg signing key fingerprint embedded in manifest body.

### 7.2 - xfstests harness integration

Add `beamfs-bench xfstests` subcommand running generic/{001,002,
010,098,257}. Required for sub-step 9 of INLINE-MULTIBLOCK and
for stage 4 closure (RFC v4 readiness, cf roadmap.md).

### 7.3 - checkpatch strict gate in pipeline

Add Phase 0.25 between lockstep (0.2) and bitbake (0.3) :
checkpatch.pl --strict on all .c/.h, fail closed if any new
warning vs baseline. Currently checkpatch is run manually.

### 7.4 - Pipeline self-test

Add a meta-test that intentionally creates a stale-code scenario
(e.g. revert one source file in yocto layer without touching
~/git/beamfs/) and verifies that pipeline Phase 0.2 lockstep
detects the divergence and aborts. Proves the pipeline is
actually fail-closed, not just exit-0-prone.

### 7.5 - bring_cluster_up reuse vs deletion

Currently `lifecycle.rs::bring_cluster_up` was deleted (orphan
after pipeline absorbed it). All sub-helpers (define_missing_vms,
destroy_all_vms, start_network, start_all_vms, wait_ssh_ready_
parallel) remain pub. If a future use case needs the legacy
compound, recreate it as `lifecycle::cold_start_cluster()` with
clearer scoping.

### 7.6 - SBOM stronger than current Yocto SPDX

Current image embeds Yocto SPDX (do_create_image_sbom_spdx).
For MIL-grade audit, link manifest -> Yocto SPDX explicitly :
copy SPDX tarball into Documentation/runs/manifest-<stamp>-sbom.tar.gz
and reference its sha256 in manifest JSON.

---

**End of TODO list.** Update by appending; never delete completed
items in-place - move to a `## Done` section at the bottom with date.
