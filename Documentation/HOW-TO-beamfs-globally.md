# HOW-TO beamfs Globally

This document is the **canonical reference for the full lifecycle of a
beamfs ecosystem code change**, from upstream source modification down
to a signed R19 manifest. It exists because every multi-product cycle
without a written procedure has degraded into ~60% wasted work :
silent `git mv` failures, untracked sources never committed, recipes
that don't compile, system binaries left at stale versions, R19
runs that test the wrong stack.

If you find yourself improvising any of the steps below, please
patch this file rather than re-deriving the procedure.

**Audience**: any operator (human or AI session) tasked with
delivering an end-to-end beamfs ecosystem change up to R19 proof.

**Companion documents**:
- `Documentation/threat-model.md` -- defines what the architecture
  must achieve.
- `Documentation/known-limitations.md` -- factual record of
  unresolved limitations.
- `~/git/beamfs-bench/Documentation/HOW-TO-BUILD-beamfs-bench.md` --
  bench-only release procedure (subset of section F below).
- `~/git/emufi/Documentation/v0.3.0-attack-design.md` -- example of
  feature-spec doc shape.

---

## 1. System overview

The beamfs ecosystem is **four products + two integration points**.

### 1.1 Products

| Product       | Repo                          | Role                                          |
| ------------- | ----------------------------- | --------------------------------------------- |
| beamfs        | `roastercode/beamfs`          | EM-resilient out-of-tree Linux filesystem     |
| radfi         | `roastercode/radfi`           | Legacy SEU bit-flip injector (kprobe)         |
| emufi         | `roastercode/emufi`           | Successor MBU/SEFI/burst injector (kprobe)    |
| beamfs-bench  | `roastercode/beamfs-bench`    | Rust orchestrator + worker.sh on the VMs      |

All four repos are **PRIVATE** until v1.0.0 of each. Visibility flips
post-publication of the corresponding paper + Zenodo DOI.

### 1.2 Integration points

| Integration       | Repo                          | Role                                          |
| ----------------- | ----------------------------- | --------------------------------------------- |
| Yocto integration | `roastercode/yocto-beamfs`    | Recipes for kernel modules + image            |
| Gentoo overlay    | `roastercode/beamfs-overlay`  | ebuilds for `sys-fs/beamfs-bench`             |

`yocto-beamfs` builds the **canonical .ext2 image** that hosts beamfs
+ injector modules + userspace tools, deployed to 4 VMs.
`beamfs-overlay` packages **`beamfs-bench`** as a Gentoo binary on
the host (spartian-1) that orchestrates the VMs.

### 1.3 Topology

```
spartian-1 (Gentoo)               4 libvirt VMs (qemu aarch64)
+------------------+               +-------------------------+
| /usr/bin/        |  ssh + scp    | beamfs-master   .56.10  |
|   beamfs-bench   | -----------> | beamfs-compute01 .56.11 |
|                  |               | beamfs-compute02 .56.12 |
| /etc/libvirt/    |  virsh        | beamfs-compute03 .56.13 |
|   qemu/          | -----------> |                         |
+------------------+               +-------------------------+
```

The bench binary lives on the host. The worker.sh script (embedded
in the bench via `include_str!`) is `scp`-deployed to each VM at
runtime.

### 1.4 Canonical image deployment

The image `hpc-arm64-research-beamfs-qemuarm64.ext2` (4096 MB) is the
**only** thing each VM disk gets. It is byte-identical across the 4
VMs. Per-VM hostname differentiation happens via kernel cmdline
`beamfs.hostname=<name>` set in the libvirt XML.

---

## 2. R-rules summary

The procedure below assumes you have read and accepted the following
hard rules. They are non-negotiable. Source : Aurélien's standing
recadrage framework.

| Rule | Subject                          | Brief                                                   |
| ---- | -------------------------------- | ------------------------------------------------------- |
| R8   | Clean working trees              | No commit on dirty tree, untracked tolerated only outside scope |
| R9   | Lockstep                         | Source repo and integration repo bumped in same session |
| R10  | Defensive backups                | `cp` before any destructive `python3 patch.py`          |
| R14  | Visibility audit before push     | `git --no-pager remote -v`, never push to public by accident |
| R16  | No em-dashes in our docs         | Replace `\u2014` by ASCII `--`                          |
| R19  | Pipeline gate                    | Full multifs run before any production claim            |
| R23  | Yocto canonical                  | The .ext2 produced by Yocto is the only valid runtime   |
| R27  | Byte-exact patches               | `python3` patches with `assert count==1` anchors        |
| R29  | GPG-signed commits               | Interactive preauth in TTY, then 3600s cache            |
| R30  | Pager neutralization             | `git --no-pager`, `gh --no-pager`, `journalctl --no-pager` |
| R31  | Byte-identity .ext2              | The 4 VMs disk start byte-identical                     |
| R36  | Tarball-canonical                | R19 emits a signed tarball of all forensics             |

The two most often forgotten in practice :
- **R8** : a `?? recipes-kernel/...` in `git status` means a previous
  `git mv` failed silently. Untracked staging directories are NOT
  innocent.
- **R29** : `gpg --batch --pinentry-mode loopback` does NOT populate
  the agent cache. Interactive `git commit -S` once per 3600s.

---

## 3. The full rebuild cycle

The cycle has **7 phases**. Skipping any phase produces an
inconsistent system and an unprovable R19.

```
A. Pre-flight                        (5 min)
B. Source change in injector / FS    (variable)
C. Sandbox bitbake validation        (5 min)
D. Apply on real repo + tag + push   (10 min)
E. Yocto recipe bump                 (5 min)
F. beamfs-bench worker patch + bump  (15 min)
G. Image rebuild + R31 + R19 full    (30-60 min)
```

Each phase has a **gate** : DO NOT enter the next phase until the
current gate is green. The gates exist because of past failures.

### 3.1 Phase A : Pre-flight

**Goal** : working trees clean, GPG cache populated, VMs in known state.

**Gate** : all four repos clean, gpg-agent state=1, 4 VMs `shut off`.

```bash
for r in beamfs emufi radfi beamfs-bench yocto-beamfs ; do
    cd ~/git/$r
    echo "=== $r ==="
    git --no-pager status --short
    git --no-pager branch --show-current
done
gpg-connect-agent 'KEYINFO --list' /bye | head -2
sudo virsh list --all | grep beamfs
```

The branch column matters : `yocto-beamfs` is typically on
`diag/double-free-block`, others on `main`. Document the deviation
in your session notes.

If gpg state=P (no preauth) :
```bash
echo "preauth" | gpg -S --output /tmp/preauth.sig -
rm /tmp/preauth.sig
# pinentry will pop up at the TTY ; cache populated for 3600s
```

If a working tree is dirty for non-scope reasons (e.g. an untracked
PDF in `~/git/emufi/papers/`), document it and proceed; do NOT
include it in scope.

### 3.2 Phase B : Source change in injector / FS

**Goal** : modify the kernel C sources of beamfs / radfi / emufi.

**Gate** : changes are byte-localized via `python3` patch scripts
with `assert count==1` anchors (R27), backed up via `cp` (R10).

**Always** :
1. Create the patch script **first**, in a sandbox dir
   (`/tmp/sprint-XYZ/` or `/home/claude/`).
2. Test it on a **copy** of the source files in that sandbox.
3. Validate brace balance per file with python : open count == close count.
4. Only then proceed to phase C.

**Anchor patterns that work** :
- Match the surrounding 3-5 lines, not a single line.
- Avoid anchors that contain the version string (which you are
  bumping anyway).
- For includes : match the **adjacent** include too, e.g.
  `'#include <linux/atomic.h>\n#include "emufi.h"'` rather than
  just `#include "emufi.h"`.

**Idempotency guards that DON'T work** :
- `if X not in content :` is wrong if X appears later in the file
  for unrelated reasons. Example : adding `#include <linux/ktime.h>`
  near line 16 with a `if "linux/ktime.h" not in content` skipped
  the add because the same header was already included **at line
  196** in a different code block, breaking the build because the
  helper using `ktime_get_ns()` was at line 89, before line 196.

**Right pattern** : check for the **exact desired final state** :
```python
if EXACT_TARGET_STRING in content :
    print("OK: already in canonical state, idempotent skip")
else :
    assert content.count(OLD_ANCHOR) == 1, "anchor not unique"
    content = content.replace(OLD_ANCHOR, NEW_BLOCK, 1)
    target.write_text(content)
```

### 3.3 Phase C : Sandbox bitbake validation

**Goal** : prove the patched sources cross-compile aarch64 BEFORE
touching the real repo or the real Yocto recipe.

**Gate** : bitbake of a **temp recipe** succeeds, `emufi.ko` (or
`radfi.ko`, or `beamfs.ko`) is produced as ELF aarch64 with the
expected version embedded.

The pattern :
1. Create temp dir `/tmp/<product>-vX.Y.Z-test-<TS>/`.
2. Copy the canonical sources from `~/git/<product>/src/`.
3. Apply the patches in this temp dir (NOT in `~/git/<product>/`).
4. Verify brace balance + checkpatch on the patched files.
5. Sync the temp dir into a **new temp Yocto recipe**
   `recipes-kernel/<product>/files/<product>-X.Y.Z-test/`.
6. Create matching `<product>-module_X.Y.Z-test.bb`.
7. `bitbake -c cleansstate <product>-module` then `bitbake <product>-module`.
8. Verify the resulting `.ko` ELF + embedded version.
9. **Discard or rename** the temp recipe before phase E.

**Common cross-compile failures to check at this stage** :
- `implicit declaration of function 'X'` -> missing `#include` for X.
  Add it at the **top** of the file, regardless of whether X's
  header is already included elsewhere in the file.
- `error: 'STRUCT_FIELD' undeclared` -> kernel API name changed
  between Yocto kernel and host kernel. Check the kernel staging
  source for the correct symbol.
- `space required after that ','` (checkpatch ERROR) -> cosmetic,
  but BLOCK promotion to mainline. If your patch did not introduce
  the offending line, you do NOT need to fix it ; only refuse if
  your own diff introduced new ERRORs.

**Host-only `make` against Yocto kernel will FAIL** : you cannot
build aarch64 kernel modules with the host x86_64 toolchain.
The sandbox MUST go through `bitbake`.

### 3.4 Phase D : Apply on real repo + commit + tag + push

**Goal** : promote the validated patches from the sandbox to
`~/git/<product>/` and tag a release.

**Gate** : `git tag -s -a vX.Y.Z` pushed to origin, and
`diff -q sandbox/ ~/git/<product>/src/` is silent (byte-identical).

Procedure :
1. R10 backup `~/git/<product>/src/<product>-X.Y.Z/` to `/tmp/<product>-pre-VX-backup-<TS>/`.
2. Apply the patches via `python3` from the **outside-the-repo**
   staging tarball or the original Downloads location.
3. Verify diff stats + byte-identity to sandbox.
4. Decide commit decomposition. Defaults :
   - **One feat commit** for the feature(s) of the bump (multiple
     features in one commit if they're conceptually one bump).
   - **One docs commit** for `Documentation/` additions.
   - **No fix commit** unless a fix-up was needed (then split it).
5. Each commit message MUST :
   - State the scope `(<product>): ...` in the title.
   - Reference cross-compile validation done in phase C with the
     `.ko` size + sha1 BuildID.
   - End with `Signed-off-by: Aurelien DESBRIERES <aurelien@hackers.camp>`
     and `Assisted-by: Claude:claude-opus-4-7`.
6. `git tag -s -a vX.Y.Z -m "..."` then `git push origin <branch>`
   then `git push origin vX.Y.Z`.

**Common gotchas at this phase** :
- ROADMAP.md may already reference the new version in a planning
  table. Don't duplicate -- check first with `grep "vX.Y.Z" ROADMAP.md`.
- `~/Downloads/` is the typical drop location for AI-session
  tarballs ; don't assume the patches are in `~/git/<product>/`
  itself.

### 3.5 Phase E : Yocto recipe bump

**Goal** : promote the bitbake-validated temp recipe to the real
versioned recipe in `yocto-beamfs`.

**Gate** : `recipes-kernel/<product>/<product>-module_X.Y.Z.bb` and
`recipes-kernel/<product>/files/<product>-X.Y.Z/` are committed and
pushed, the **temp** recipe and old versioned dirs deleted.

**This phase has the highest historical failure rate.** The errors :

#### E.1 The untracked source trap

If the temp recipe sources were created in phase C as untracked
files (typical : you `cp -r` into the recipe files dir), then
**`git mv` will fail silently** :
```
fatal: source directory is empty, source=recipes-kernel/...
fatal: not under version control, source=recipes-kernel/.../X.bb
```

The shell loop continues anyway, the `git rm` of the OLD versioned
files succeeds, and you end up committing a **destructive commit**
that deletes the recipe without adding a replacement.

**Fix : NEVER `git mv` an untracked source.** Either :
- `git add` the temp recipe FIRST (to make it tracked), then `git mv`.
- OR : use shell `mv` (not `git mv`) to rename, then `git add` the
  result. This is what works around untracked sources.

The defensive sequence :
```bash
# ASSUME : files/<product>-X.Y.Z-test/ and <product>-module_X.Y.Z-test.bb
#          exist on disk but are untracked (created in phase C).

# 1. shell mv to drop the -test suffix.
mv recipes-kernel/<product>/files/<product>-X.Y.Z-test \
   recipes-kernel/<product>/files/<product>-X.Y.Z
mv recipes-kernel/<product>/<product>-module_X.Y.Z-test.bb \
   recipes-kernel/<product>/<product>-module_X.Y.Z.bb

# 2. patch the .bb to drop the -test suffix from SRC_URI / S strings.
sed -i 's|file://<product>-X.Y.Z-test/|file://<product>-X.Y.Z/|g' \
       recipes-kernel/<product>/<product>-module_X.Y.Z.bb
sed -i 's|WORKDIR}/<product>-X.Y.Z-test|WORKDIR}/<product>-X.Y.Z|g' \
       recipes-kernel/<product>/<product>-module_X.Y.Z.bb

# 3. git rm the old versioned recipe + sources.
git rm -r recipes-kernel/<product>/files/<product>-OLD/
git rm    recipes-kernel/<product>/<product>-module_OLD.bb

# 4. git add the new ones (NOT git mv).
git add recipes-kernel/<product>/files/<product>-X.Y.Z/
git add recipes-kernel/<product>/<product>-module_X.Y.Z.bb

# 5. THEN commit.
git --no-pager status --short  # verify A entries match new + D entries match old
git commit -S -m "..."
```

#### E.2 The Yocto Makefile divergence

The Yocto recipe carries a flavoured `Makefile` (commit `e79ba0b`
on `yocto-beamfs`) that adapts the out-of-tree Makefile for
Yocto cross-compile (`KERNEL_SRC=$(STAGING_KERNEL_DIR)`,
`O=$(KERNEL_BUILD_ARTIFACTS)`).

**Do NOT overwrite it** when syncing sources from `~/git/<product>/src/`.
Sync everything except the Makefile :
```bash
for f in <product>.h <product>_main.c <product>_inject.c \
         <product>_hooks_blk.c <product>_hooks_fs.c Kbuild ; do
    cp ~/git/<product>/src/<product>-OLD/$f \
       recipes-kernel/<product>/files/<product>-X.Y.Z/
done
# Makefile stays as-is (Yocto-flavoured).
```

Or copy the Makefile from the previous Yocto recipe :
```bash
cp recipes-kernel/<product>/files/<product>-OLD/Makefile \
   recipes-kernel/<product>/files/<product>-X.Y.Z/Makefile
```

#### E.3 Branch awareness

`yocto-beamfs` is typically on `diag/double-free-block`, **not** on
`main`. Always `git --no-pager branch --show-current` before
committing. Pushing to the wrong branch on a private repo is recoverable
but takes 10 minutes you don't want to spend.

### 3.6 Phase F : beamfs-bench worker.sh patch + bump + ebuild + emerge

**Goal** : if the kernel module change requires new debugfs entries,
patch `worker.sh` to push them, bump beamfs-bench version,
release a new ebuild, emerge it.

**Gate** : `beamfs-bench --version` shows the new version on the
host, and `/usr/bin/beamfs-bench` mtime matches the emerge.

This phase follows `Documentation/HOW-TO-BUILD-beamfs-bench.md`
section 5 in the bench repo. Summary :

1. Patch `src/worker.sh` (the embedded shell that arms the
   injector). Add `[ -e ${INJECTOR_DBG}/X ] && echo $X | sudo tee ...`
   for each new debugfs entry. **Both** arming sites
   (multifs ~line 245, metadata cluster ~line 497) must be patched.
2. Bump `Cargo.toml` version, run `cargo check` to regen Cargo.lock.
3. Commit + tag + push beamfs-bench.
4. In overlay `/var/db/repos/beamfs-overlay/sys-fs/beamfs-bench/` :
   - `cp <prev>.ebuild <new>.ebuild`.
   - `sed -i 's/EGIT_COMMIT="vOLD"/EGIT_COMMIT="vNEW"/'`.
   - Update `pkg_postinst()` version line + release notes block.
   - `sudo ebuild <new>.ebuild manifest`.
   - `git add` (as aurelien, NOT as root) + commit (signed) + push.
5. Emerge with mask 9999 :
   ```
   echo "=sys-fs/beamfs-bench-9999" | sudo tee /etc/portage/package.mask/beamfs-bench-9999
   sudo emerge -1 sys-fs/beamfs-bench
   sudo rm /etc/portage/package.mask/beamfs-bench-9999
   ```
6. Verify : `beamfs-bench --version`.

**Common pitfalls** :
- `git -c user.email=... -c user.name=... commit -S` is needed in
  the overlay because the overlay repo doesn't have local
  `user.email`. Skipping these flags creates a commit with the
  default (root, sometimes), which fails GPG ownership.
- The overlay Manifest is **gitignored** (see `.gitignore` in the
  overlay). Don't try to commit it.
- Leftover `/etc/portage/package.mask/beamfs-bench-9999` will
  silently skip 9999 selection on next emerges. Always remove it
  even on emerge failure.
- The `release notes` block in the ebuild may have a `See:` URL
  that mentions an old version (e.g. `tag/v0.7.2` while the file
  is `0.7.4.ebuild`). Cosmetic but please fix at next bump.

### 3.7 Phase G : Image rebuild + R31 redeploy + R19 full

**Goal** : produce the canonical .ext2 with the new injector + new
worker baked in, deploy to the 4 VMs, run R19 and emit a signed
manifest.

**Gate** : `manifest-<TS>.json.asc` GPG-signed, present in
`~/git/yocto-beamfs/Documentation/runs/`, R19 exit 0,
dmesg clean across all 4 nodes.

#### G.1 Image rebuild

```bash
cd ~/yocto/poky
source oe-init-build-env build-qemu-arm64

# Force rebuild from new sources (cleansstate cascades to image).
bitbake -c cleansstate <product>-module
bitbake -c cleansstate hpc-arm64-research-beamfs
bitbake hpc-arm64-research-beamfs

# Verify image was rebuilt.
ls -la tmp/deploy/images/qemuarm64/hpc-arm64-research-beamfs-qemuarm64.ext2

# Verify the new module is inside.
TMPMNT=/tmp/check-image-$(date +%H%M%S)
mkdir -p $TMPMNT
sudo mount -o loop,ro tmp/deploy/images/qemuarm64/hpc-arm64-research-beamfs-qemuarm64.ext2 $TMPMNT
strings $TMPMNT/lib/modules/7.0.3/updates/<product>.ko | grep version=
sudo umount $TMPMNT && rmdir $TMPMNT
```

The `cleansstate` of the **image** is critical : without it,
Yocto reuses the previous rootfs cache and the new module never
makes it into the .ext2 even though the recipe was bumped.

#### G.2 R31 redeploy : 4 VMs byte-identical

Each VM disk is replaced with a fresh copy of the canonical .ext2 :
```bash
for vm in beamfs-master beamfs-compute01 beamfs-compute02 beamfs-compute03 ; do
    sudo virsh destroy $vm 2>/dev/null
    sudo cp tmp/deploy/images/qemuarm64/hpc-arm64-research-beamfs-qemuarm64.ext2 \
            /var/lib/libvirt/images/hpc-arm64/$vm.ext2
    sudo chown qemu:qemu /var/lib/libvirt/images/hpc-arm64/$vm.ext2
done
sudo sha256sum /var/lib/libvirt/images/hpc-arm64/beamfs-*.ext2
# All four sha256s MUST match (R31).
```

If the four sha256s don't match, your VMs will diverge at first
boot and the R19 manifest will be unsigned/invalid.

#### G.3 R19 full

```bash
cd ~/git/yocto-beamfs   # R24 invocation point
beamfs-bench full --auto-confirm 2>&1 | tee /tmp/r19-vXYZ-$(date +%Y%m%d-%H%M%S).log
echo "exit code : $?"
```

R19 takes 5-30 min depending on USB count. Final output :
- `Documentation/runs/beamfs-bench-analyse-full-<TS>/` (forensics tarball).
- `Documentation/runs/manifest-<TS>.json.asc` (GPG-signed manifest).
- `Documentation/runs/beamfs-bench-multifs-<TS>/` (multifs head-to-head).
- regression report vs baseline.

**If R19 fails on Phase 0.0bis (`code_analysis`)** : your working
trees are dirty (clippy/checkpatch errors). Fix and re-run.

**If R19 fails on Phase 0.1 (`clean trees`)** : you have unpushed
commits. Push and re-run.

**If R19 succeeds but `multifs-synthesis.md` shows hash unchanged
on both ext4 and beamfs** : your `target_block` is hitting the wrong
block (typical with ext4 : flips land on bitmap). Check
`forensics-beamfs-compute01/injector-counters.log` to see which
counters incremented and which entries debugfs has populated.
This is precisely what the `TARGET_STRUCT` + `TARGET_STRUCT_BLOCK_NO`
mechanism (rv4-4 in emufi v0.3.0) was designed to fix.

#### G.4 5-case discrimination matrix (only available with emufi v0.3.0+ and bench v0.7.4+)

For the paper's empirical corroboration, R19 is run **5 times**
with different env vars set in the bench invocation :

| Case  | INJECTOR | FLIP_LOCALITY | BURST_SYMBOLS | TARGET_STRUCT | SEFI_PROBABILITY | Expected ext4 / beamfs                |
| ----- | -------- | ------------- | ------------- | ------------- | ---------------- | ------------------------------------- |
| sb-1  | emufi    | 0 (RANDOM)    | -             | 5 (DATA)      | 0                | corrupt / RS recovery                 |
| sb-2  | emufi    | 4 (CODEWORD)  | 9             | 5 (DATA)      | 0                | EIO / UNCORRECTABLE in RAJ            |
| sb-3  | emufi    | 0 (RANDOM)    | -             | 3 (BITMAP)    | 0                | mount fail / RS recovery on remount   |
| sb-4  | emufi    | 4 (CODEWORD)  | 9             | 3 (BITMAP)    | 0                | mount fail / EIO + RAJ entry          |
| sb-5  | emufi    | -             | -             | 0 (NONE)      | 100000           | EIO on read / EIO on read (FEC bypass)|

This matrix is the empirical analogue of the paper section IV
recovery surface theorem. It is the **minimum dataset** for the
"Empirical corroboration" section.

---

## 4. Real failure modes observed (and how to avoid them)

### 4.1 Silent `git mv` on untracked sources (phase E)

**Symptom** : `git mv X Y` prints `fatal: ... not under version control`,
shell continues, subsequent `git rm` / `git commit` succeeds, and
the result is a destructive commit with no replacement files.

**Cause** : `git mv` requires source to be tracked. Phase C
typically creates the temp recipe as untracked.

**Fix** : use shell `mv` for promotion, then `git add` the result.
See section 3.5 E.1.

**Detection** : after the bump commit, `ls
recipes-kernel/<product>/` should show the new versioned files.
If you see only an empty `files/` and no `.bb`, the bump was
destructive.

### 4.2 The `if X not in content` idempotency trap (phase B)

**Symptom** : a `#include` was supposed to be added at the top of
the file, but it's somehow at line 196 instead. Compile fails
with `implicit declaration of function`.

**Cause** : the patch's idempotency guard `if "#include <X>" not in
content :` matched an existing occurrence elsewhere in the file
(e.g. inside an `#ifdef` block, or in a function added later)
and skipped the insertion at the top.

**Fix** : check for the **exact final canonical state** as the
guard, not for substring presence. See section 3.2 last block.

### 4.3 `bitbake` apparent success but stale artefacts (phase G)

**Symptom** : `bitbake hpc-arm64-research-beamfs` exits 0 but the
new module is not in the .ext2.

**Cause** : the rootfs cache was reused. `cleansstate` was
applied to the module recipe but not to the image recipe.

**Fix** : `bitbake -c cleansstate hpc-arm64-research-beamfs`
**before** the rebuild. Section 3.7 G.1.

### 4.4 The 543-flip / 0-diff paradox (phase G)

**Symptom** : R19 reports 543 flips on ext4 at probability=1M,
but `cat`-then-sha256 of the target file is unchanged.

**Cause** : flips are uniformly distributed over the bio's first
segment. Most bios are bitmap or directory-block I/Os, NOT the
target file's data block. The 543 flips are real but they're
landing on metadata that isn't in the file we hash.

**Fix** : enable `TARGET_STRUCT=5 (DATA_BLOCK)` and let the worker
auto-fill `TARGET_STRUCT_BLOCK_NO` from filefrag (added in
beamfs-bench v0.7.4 + emufi v0.3.0 rv4-4). Without these, you
**cannot** discriminate ext4 from beamfs in a multifs run.

### 4.5 GPG ownership in overlay commits (phase F)

**Symptom** : `git commit -S` in `/var/db/repos/beamfs-overlay/`
fails with `gpg: WARNING: unsafe ownership on homedir`.

**Cause** : the overlay was last touched by `sudo`, leaving
`.git/index` owned by root. `git commit` runs as aurelien but
GPG agent is bound to the calling user, not to root's homedir.

**Fix** : `sudo chown aurelien:aurelien .git/index` then commit
as aurelien with explicit `-c user.email=... -c user.name=...`.

### 4.6 Wrong toolchain in out-of-tree validation (phase C variant)

**Symptom** : `make KERNEL_SRC=...` in the sandbox fails with
`fatal error: asm/compiler.h: No such file or directory`.

**Cause** : you used the **host** `gcc` (x86_64) against
**aarch64** kernel headers.

**Fix** : never use host `make` for module validation. Always
go through `bitbake <module-name>` which uses the cross-toolchain
provided by Yocto. Section 3.3.

### 4.7 Injector default drift between radfi and emufi

**Symptom** : R19 with `--injector emufi` reports CALL_DELTA=0 and
FLIP_DELTA=0 on the beamfs target volume despite probability=1M ppm.
Same configuration with `--injector radfi` produces 25+ flips.

**Cause** : emufi v0.3.0 defaulted `inject_on_read = false` in
`emufi_state_init`. radfi has historically defaulted to `true`.
The harness `worker.sh` never pushed `inject_on_read` explicitly,
relying on the injector default. Switching the default injector
silently changed the harness contract.

**Detection** : check
`/sys/kernel/debug/<injector>/inject_on_read` in the forensics
output (`forensics-beamfs-compute01/injector-counters.log`). It
must read `Y` for read-driven attack workflows. If `N`, the
attack is silently inert.

**Fix (in lockstep)** :
  - emufi v0.3.1 aligns default to `inject_on_read = true` to
    match radfi.
  - beamfs-bench v0.7.6 pushes `inject_on_read=1` to debugfs
    unconditionally at every attack arming site
    (`worker.sh` line ~244 multifs, line ~497 metadata cluster)
    as defense-in-depth.

**Lesson** : when two injectors share an interface, every entry
that the harness relies on must either be explicitly pushed by the
harness, or have a documented and identical default across all
injectors. Implicit default contracts between products break
silently when one product changes its default.

---

## 5. Quick reference commands

### 5.1 GPG preauth
```
echo "preauth" | gpg -S --output /tmp/preauth.sig - && rm /tmp/preauth.sig
```

### 5.2 Working tree audit (all 5 repos)
```
for r in beamfs emufi radfi beamfs-bench yocto-beamfs ; do
    cd ~/git/$r && echo "=== $r $(git --no-pager branch --show-current) ===" \
                && git --no-pager status --short
done
```

### 5.3 Sandbox bitbake validation
```
# Pre-req : sources patched in /tmp/<product>-vX.Y.Z-test-<TS>/src/<product>-OLD/
TEMP=$(ls -1dt /tmp/<product>-vX.Y.Z-test-* | head -1)
cd ~/git/yocto-beamfs/recipes-kernel/<product>/
mkdir -p files/<product>-X.Y.Z-test
cp $TEMP/src/<product>-OLD/*.{c,h} files/<product>-X.Y.Z-test/
cp $TEMP/src/<product>-OLD/Kbuild files/<product>-X.Y.Z-test/
cp files/<product>-OLD/Makefile files/<product>-X.Y.Z-test/   # Yocto-flavoured
cp files/<product>-OLD/COPYING files/<product>-X.Y.Z-test/
cp <product>-module_OLD.bb <product>-module_X.Y.Z-test.bb
sed -i 's|file://<product>-OLD/|file://<product>-X.Y.Z-test/|g' \
       <product>-module_X.Y.Z-test.bb
sed -i 's|WORKDIR}/<product>-OLD|WORKDIR}/<product>-X.Y.Z-test|g' \
       <product>-module_X.Y.Z-test.bb
cd ~/yocto/poky && source oe-init-build-env build-qemu-arm64
bitbake -c cleansstate <product>-module
bitbake <product>-module
```

### 5.4 R31 redeploy (4 VMs)
```
cd ~/yocto/poky/build-qemu-arm64
IMG=tmp/deploy/images/qemuarm64/hpc-arm64-research-beamfs-qemuarm64.ext2
for vm in beamfs-master beamfs-compute01 beamfs-compute02 beamfs-compute03 ; do
    sudo virsh destroy $vm 2>/dev/null
    sudo cp $IMG /var/lib/libvirt/images/hpc-arm64/$vm.ext2
    sudo chown qemu:qemu /var/lib/libvirt/images/hpc-arm64/$vm.ext2
done
sudo sha256sum /var/lib/libvirt/images/hpc-arm64/beamfs-*.ext2
# All 4 hashes MUST be identical.
```

### 5.5 R19 full
```
cd ~/git/yocto-beamfs
beamfs-bench full --auto-confirm 2>&1 | \
    tee /tmp/r19-$(date +%Y%m%d-%H%M%S).log
echo "exit : $?"
```

---

## 6. When in doubt

If any phase produces an unexpected result, **stop**. Do not
"continue and see". The R-rules exist because every "let's see"
in the past has cost more time than the careful step-by-step.

In order of priority :
1. Read the actual log (not the tail), find the first `error:` or
   `fatal:` line.
2. Verify file existence and contents at the path mentioned.
3. Verify branch + working tree state of all involved repos.
4. Walk back through this document's section that names that
   phase, find the matching gotcha in section 4.
5. Only then propose a fix, with a specific commit message that
   names the phase and the gotcha.

The most expensive failure pattern observed in past sessions :
patching downstream layers (recipe, ebuild, image) before the
upstream patch (kernel module sources) has cleared phase C
validation. **Phase C is non-negotiable.**

---

## 7. Document maintenance

This document is updated whenever a new failure mode is observed
during a real cycle. The history of failure modes is part of the
audit value of the document. Do NOT remove entries from section 4
when the underlying cause is fixed in a later version of the
tooling -- annotate them as `(fixed in X.Y.Z)`. The procedure
must remain readable to operators using older toolchains.

Last updated : 2026-05-06.
First written after : full rebuild cycle 2026-05-06 emufi v0.2.1
-> v0.3.0 surgical attack suite + beamfs-bench v0.7.3 -> v0.7.4
worker integration + Yocto image rebuild + R31 + R19 full, where
~60% of session time was lost to silent `git mv` failures,
include-ordering bugs, and stale rootfs cache. This document
exists to make those losses one-time costs.
