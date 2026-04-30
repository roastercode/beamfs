# BEAMFS — TODO list (Claude-aware deferred work)

> **CLASSIFICATION INTERNAL — NEVER PUSH TO PUBLIC GITHUB**
>
> Linked from `context/STATUS.md`. Versioned only on
> `roastercode/beamfs-devel` (private), via `.gitignore` exception.
>
> **CLAUDE — READ FIRST**: see `context/context-recadrage.md`
> for the operational contract before touching anything in this list.

---

## TODO 1 — Lowercase normalization (deferred, manual review required)

**Why deferred** : 350 occurrences of `BEAMFS` in `.md`/`.txt` and
~950 in `.c`/`.h` strings/comments across `beamfs` and `yocto-beamfs`.
The kernel.org convention is lowercase `beamfs` in prose and dmesg
strings (cf. `btrfs`, `ext4`, `squashfs`). However, automated
substitution would damage the following non-trivial cases:

### 1.1 — Cases that must NEVER be lowercased

- Macros: `BEAMFS_MAGIC`, `BEAMFS_BLOCK_SIZE`, `BEAMFS_DATA_*`,
  `BEAMFS_VERSION_*`, `BEAMFS_FORMAT_V4`, `BEAMFS_RS_*`,
  `BEAMFS_FEAT_*`, `BEAMFS_SB_RS_*`, `BEAMFS_INODE_*`,
  `BEAMFS_INDIRECT_PTRS`, `BEAMFS_SUBBLOCK_*` — all C identifiers
  in ALL_CAPS (kernel coding style).
- Magic literal: `"BEAM"` in `0x4245414D` is a 4-byte ASCII magic,
  not a name. Never lowercase.
- All content under `papers/` (`.tex`, `.bib`, `.aux`, build artefacts)
  — formal scientific publication, published on Zenodo with fixed DOI.
  No textual modifications.
- Tags and commit messages already pushed (signed history).
- File `Documentation/format-v4.md` is the on-disk format
  specification, normative, referenced by the macros it documents.
  Prose like "BEAMFS v4 is the on-disk format" must stay aligned
  with `BEAMFS_FORMAT_V4` macro.
- File `Documentation/EMPIRICAL-FINDINGS.md` (title `# BEAMFS v1 ...`):
  dated session document, historical reference, do not retitle.

### 1.2 — Cases to lowercase, by hand, file-by-file

| File | Action | Risk |
|---|---|---|
| `README.md` (beamfs public) | review only — this is the public face, casing is part of branding | high — discuss before |
| `Documentation/2026-04-28-validation-session.md` | lowercase prose mid-sentence, keep `FTRFS↔BEAMFS` sigle in tables | low |
| `Documentation/design.md` (titles excluded) | lowercase prose body | medium — many macro mentions |
| `Documentation/multifs-bench-2026-04-30.md` | lowercase the `\| BEAMFS \|` table entries to align with `\| ext4 \|` | low |
| `Documentation/system-architecture.md` | lowercase prose body | low |
| `Documentation/testing.md` | lowercase prose | low |
| `Documentation/threat-model.md` | lowercase prose, keep formal references uppercase | medium |
| `Documentation/roadmap.md` | lowercase prose | low |
| `Documentation/known-limitations.md` | lowercase prose | low |
| `.c`/`.h` SPDX-like headers (`* BEAMFS — XXX`) | lowercase to `* beamfs — XXX` per mainline (`* btrfs ...`) | low |
| `.c`/`.h` `pr_*` log strings: `"beamfs: ..."` prefix is ALREADY lowercase. Only the descriptive text after the prefix needs review (e.g. `"BEAMFS Beam Electromagnetic"` → `"beamfs"` only) | low |

### 1.3 — Concrete examples to handle manually

super.c:	pr_info("beamfs: module loaded (BEAMFS Beam Electromagnetic File System, EM resilience)
");
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
-> "beamfs (Beam Electromagnetic File System, EM resilience)"


### 1.4 — Recommended approach when bandwidth allows

1. One commit per file (auditable, revertable).
2. `git diff` review before each commit.
3. Skip `papers/`, all `BEAMFS_*` macros, `"BEAM"` magic literal,
   `EMPIRICAL-FINDINGS.md` title, `format-v4.md` normative refs.
4. Test build (`bitbake beamfs-module`) after each `.c`/`.h` commit.
5. Run `beamfs-bench analyse --scope=full` (ex-`Tir-analyse-multifs.sh`) after the kernel-source pass to confirm
   no regression.

---

## TODO 2 — beamfs-bench (Rust unified harness)

**Status (2026-04-30)** : DONE for `multifs` + `analyse` scopes.
The 5 shell scripts that originally lived in `~/git/yocto-beamfs/bin/`
have been assembled, augmented (cluster-wide forensics, R12 device
validation, R13 topology auto-discovery), and rassembled into a
single Rust binary `beamfs-bench`. The ancestor scripts were:

  - Tir.sh (443 lines)              -> `beamfs-bench bench` (PENDING)
  - Tir-analyse.sh (206)            -> `beamfs-bench analyse` (DONE)
  - Tir-analyse-rapide.sh (27)      -> merged into `analyse --scope=quick` (DONE)
  - Tir-multifs.sh (425)            -> `beamfs-bench multifs` (DONE)
  - Tir-analyse-multifs.sh (75)     -> `analyse --scope=full` (DONE)

Total: 1176 lines of bash assembled into a single Rust binary
`beamfs-bench` with subcommands. Validation matrix executed against
the live 4-node BEAMFS cluster on 2026-04-30 (multifs, analyse quick,
analyse standard, analyse full); 5 successful runs covering 5 FS,
3 probabilities, and cluster-wide RadFI saturation on 4 nodes
simultaneously (48 confirmed flips, 0 corruption).

**Remaining work to fully close TODO 2** :

1. Extract `beamfs-bench/` from `yocto-beamfs/` to a dedicated private
   repository `roastercode/beamfs-bench` (subtree split, keeping the
   3 commits of history). After extraction: `git rm -r beamfs-bench/`
   in yocto-beamfs and replace by `beamfs-bench/MOVED.md` pointer.

2. Move the legacy `bin/Tir-*.sh` files in `yocto-beamfs/` to
   `bin/legacy/` and add a README explaining they are kept for
   historical reference only and that `beamfs-bench` is the
   replacement. Removing them entirely is also acceptable since
   `beamfs-bench` reproduces (and extends) all their functionality.

3. Port the last unported script `Tir.sh` (443 lines, HPC iobench)
   to `beamfs-bench bench` subcommand. Multi-node by design.

4. Implement 4 new scopes that did not exist in legacy bash:
   `metadata` (Test A — superblock/inode/journal targeted attack),
   `crash` (Test B — virsh destroy mid-write + remount),
   `bitrot` (Test C — dd random on offline partition),
   `fsck` (Test D — fsck recovery post-FS_PANIC).

5. Fix the `perf record` header data_size=0 race in
   `forensics::stop_perf_master` (currently the polling on
   `pgrep -x perf` exits before perf flushes its file header).

### 2.1 — Subcommand layout

beamfs-bench multifs       # current Tir-multifs scope
beamfs-bench analyse       # forensic wrapper (dmesg + ftrace + perf)
beamfs-bench bench         # cluster perf bench (current Tir.sh scope)
beamfs-bench metadata      # NEW: Test A — metadata-targeted attack
beamfs-bench crash         # NEW: Test B — crash consistency
beamfs-bench bitrot        # NEW: Test C — bit-rot offline
beamfs-bench fsck          # NEW: Test D — fsck recovery post-FS_PANIC


Common flags: `--probs`, `--fs`, `--target`, `--out-dir`, `--ssh-key`,
`--master-ip`, `--trials`, `--report`.

### 2.2 — Crate dependencies (proposed)

- `clap` v4 — argument parsing
- `serde` + `serde_json` — JSON output for synthesis
- `ssh2` or `russh` — SSH execution to master VM
- `chrono` — timestamps
- `anyhow` + `thiserror` — error handling
- `tracing` — structured logging

### 2.3 — Estimate

- Skeleton + Cargo.toml + multifs subcommand: 1h
- Port Tir-multifs.sh → multifs.rs: 2h
- Port Tir-analyse-multifs.sh wrapping → analyse.rs: 1h
- Tests A/B/C/D: 3-4h
- Forensic capture (dmesg/ftrace/perf): 1-2h
- Validation runs: 2h

Total: 10-12h focused work.

### 2.4 — Naming policy (anti-NAK)

- Binary name: `beamfs-bench` (lowercase, hyphenated, mainline-style)
- Crate name: `beamfs-bench`
- Subcommands: lowercase verbs/nouns
- Log output: `beamfs-bench: ...` prefix (lowercase, like dmesg)
- Module structure: `src/multifs.rs`, `src/metadata.rs`, etc.

### 2.5 — Migration plan

1. Land `beamfs-bench multifs` reproducing Tir-multifs.sh exactly
   (verdicts byte-identical to reference run 20260430-141008).
2. Once parity confirmed, add `analyse` and `bench` subcommands.
3. Then add the 4 new tests (metadata, crash, bitrot, fsck).
4. After all tests pass: `git mv bin/Tir-*.sh bin/legacy/`
5. Update `Documentation/iobench-baseline-*.md` references to point to
   `beamfs-bench bench` instead of `bin/hpc-benchmark-beamfs.sh`.
6. New bench-v2 multi-capability report:
   `Documentation/multifs-bench-v2-*.md`

---

## TODO 3 — Sub-step 4 of multiblock design

Continue the 10-step INLINE-MULTIBLOCK-DESIGN.md sequence:

- Sub-step 4: `write_begin` multi-block (remove `-EFBIG`), 30 min
- Sub-step 5: `writepages` multi-folio + `writeback_folio` +
  scratch kmalloc + `i_alloc_mutex`, 3h
- Sub-step 6: `setattr` truncate, 1h
- Sub-step 7: sparse + durability tests, 30 min
- Sub-step 8: `.mmap = generic_file_mmap`, 30 min
- Sub-step 9: xfstests subset (generic/001, 002, 010, 098, 257), 2-4h
- Sub-step 10: checkpatch strict zero CHECKs, 1h

Total remaining for full multiblock: 8-10h focused work.

---

## TODO 4 — Yocto image cleanup

`yocto-beamfs/Documentation/runs/` accumulates Tir-* runs. Currently
~17 untracked directories + 5 tarballs. Decide a retention policy:

- Keep only the 3 latest validated runs (multifs-141008, multifs-193032,
  + future).
- Older runs: tarball + remove.
- Add `.gitignore` for `Documentation/runs/Tir-*` except a
  curated set.

Estimated 30 min.

---

## TODO 5 — `.bb` SUMMARY/DESCRIPTION yocto

Yocto `.bb` recipes use `BEAMFS` in SUMMARY/DESCRIPTION fields.
Yocto convention: first letter capitalized, rest lowercase
("Btrfs userspace tools" not "BTRFS userspace tools").

Files to review (~34 occurrences):
- `recipes-kernel/beamfs/beamfs-module_0.1.0.bb`
- `recipes-kernel/beamfs/mkfs-beamfs_0.1.0.bb`
- `recipes-kernel/beamfs/beamfsd_0.1.0.bb` (if exists)
- `recipes-core/images/hpc-arm64-research-beamfs.bb`

Estimated 15 min, low risk.

---

## TODO 6 — Update STATUS.md to reflect current acquis (post-substep-2-3)

Current STATUS.md is dated `2026-04-30 fin-session migration labo` and
predates sub-steps 2-3 multiblock-read. It needs an update to mention:

- `v1.5.0-rc1-substeps2-3-multiblock-read` (beamfs devel)
- `v1.5.1-context-recadrage-v1` (beamfs devel)
- Capabilities matrix section in multifs-bench report
- TODO.md (this file) as the live deferred work list

Estimated 10 min.

---

**End of TODO list.** Update by appending; never delete completed
items in-place — move to a `## Done` section at the bottom with date.
