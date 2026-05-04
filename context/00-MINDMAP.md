# beamfs - Project Mindmap

> **CLASSIFICATION INTERNAL - NEVER PUSH TO PUBLIC GITHUB**
>
> Storage : `roastercode/beamfs-devel` PRIVATE, branch `mainline-prep`.
> NOT for `roastercode/beamfs` PUBLIC.

> **CLAUDE - READ FIRST in any new session.** This file is the entry
> point for navigating the entire beamfs project. Read this BEFORE
> the recadrage, the STATUS, the TODO, the roadmap. It tells you
> where everything lives and what each artefact is for.

Last updated : 2026-05-01

---

## 1. Project at a glance

beamfs is a Linux kernel filesystem resilient to electromagnetic
perturbations (cosmic SEU, IEMI, voltage glitches, Rowhammer, NAND
charge loss, RF noise). Reed-Solomon FEC + autonomic repair on
metadata + data blocks. Target : MIL-grade safety-critical embedded
systems (DO-178C, ECSS-E-ST-40C, IEC 61508).

Scientific lineage : extends FTRFS (Fuchs et al. ARCS 2015) with a
unified electromagnetic threat model (paper v2, Zenodo 19886192).

---

## 2. Repos (lockstep)

| Repo                 | Visibility          | Role                                          |
|----------------------|---------------------|-----------------------------------------------|
| beamfs               | PUBLIC origin       | Kernel module + papers (mainline submission)  |
| beamfs-devel         | PRIVATE             | Same code + context/ (recadrage, STATUS, TODO)|
| yocto-beamfs         | PRIVATE             | Yocto layer, image canonique cluster (R23)    |
| beamfs-bench         | PRIVATE             | Rust harness for resilience bench             |
| beamfs-overlay       | PRIVATE             | Gentoo overlay (sys-fs/beamfs-bench ebuild)   |
| radfi                | PRIVATE             | EM fault injection module (Zenodo 19885777)   |

The two beamfs repos are mirrored; only context/ differs.

---

## 3. Cluster topology (R-isolation, R21)

beamfs-master    .10  vda + vdb                        orchestrator
beamfs-compute01 .11  vda + vdb + vdc..vdg (5 USB)     FS-test holder
beamfs-compute02 .12  vda + vdb                        cluster compute
beamfs-compute03 .13  vda + vdb                        cluster compute


5 USB FS targets : ext4(vdc), ext3(vdd), btrfs(vde), squashfs(vdf), beamfs(vdg).
Master never holds USB victims (rigour : transversal kernel state).

---

## 4. beamfs-bench resilience matrix

7 subcommands, 77 observations per full run :

| Class       | Subcommand | Mode              | FS  | Scenarios | Obs |
|-------------|------------|-------------------|-----|-----------|-----|
| Live SEU    | multifs    | RadFI live attack | 5   | 3 probs   | 15  |
| Cluster     | (in full)  | RadFI 4 nodes     | beamfs| 3 probs | 12  |
| Pipeline    | full       | lifecycle+all     | -   | -         | -   |
| Rest        | bitrot     | offline dd random | 5   | 4 cells   | 20  |
| Metadata    | metadata   | RadFI determinist | 5   | 4 blocks  | 20  |
| Crash       | crash      | virsh destroy mid | 4*  | 1 cell    | 5   |
| Offline     | fsck       | fsck.<fs>         | 4*  | 1 cell    | 5   |

(*) squashfs RO skipped on crash + fsck.

R19 canonical pre-push : `beamfs-bench full --auto-confirm` exit 0.

---

## 5. Documentation map

### context/ (PRIVATE recadrage actif)

context/
├── 00-MINDMAP.md              -- this file (entry point)
├── context-recadrage.md       -- R0-R37 operational contract
├── STATUS.md                  -- current state (live updated)
├── TODO.md                    -- deferred work (append only)
├── INLINE-MULTIBLOCK-DESIGN.md-- design phase 3 (active)
└── archive/                   -- historical handoffs, session notes


### Documentation/ (PUBLIC, normative)

Documentation/
├── system-architecture.md     -- cluster layout + IO path
├── threat-model.md            -- normative EM threat model
├── known-limitations.md       -- gap impl vs threat model
├── roadmap.md                 -- Phase 0-8 + Stage 1.5-5
├── format-v4.md               -- on-disk v4 (normative current)
├── format-v5.md               -- on-disk v5 (RFC-able future)
├── fsck.beamfs.md             -- offline checker design
├── testing.md                 -- test methodology
├── empirical-state.md         -- empirical state snapshot
└── archive/sessions/          -- ephemeral session reports


### papers/ (publications)

papers/
├── 2026-04-beamfs-v1/         -- Zenodo 19824442 (FTRFS rebadge)
├── 2026-04-beamfs-v2/         -- Zenodo 19886192 (INLINE RS, current)
├── 2026-04-beamfs-v3-findings/-- Working draft (cluster-wide + multi-FS)
└── 2026-04-radfi-v1/          -- Zenodo 19885777 (RadFI tool)


### yocto-beamfs/

yocto-beamfs/
├── recipes-core/images/hpc-arm64-research-beamfs.bb   -- R23 canonical
├── recipes-kernel/{linux,beamfs,radfi}/               -- kernel + modules
├── recipes-hpc/{slurm,munge,pmix}/                    -- HPC stack
└── Documentation/
├── beamfs-integration.md  -- short pointer to R23
└── archive/                -- iobench baselines


---

## 6. Key rules (R0-R37 in recadrage)

| R   | Subject                                                          |
|-----|------------------------------------------------------------------|
| R0  | No push with bugs/warnings (extends to R19)                      |
| R6  | BEAMFS_* macros uppercase, beamfs lowercase prose                |
| R12 | Anti-NAK device validation USB by-id                             |
| R13 | Cluster topology (clarified by R21)                              |
| R14 | beamfs PUBLIC origin / devel PRIVATE devel                       |
| R15 | phase=N footer in commits                                        |
| R16 | em-dash forbidden                                                |
| R17 | Tagline "beamfs - resilient filesystem"                          |
| R18 | No glob shell, no checkpatch --file on header                    |
| R19 | beamfs-bench full exit 0 obligatoire pre-push                    |
| R20 | Console output format (#######/#début/#######)                  |
| R21 | R-isolation FS-test architecture                                 |
| R22 | R-os-stack Gentoo OpenRC Wayland Sway foot                       |
| R23 | R-image-canonique hpc-arm64-research-beamfs.bb                   |
| R24 | R-CWD bench invocation from yocto-beamfs/                        |
| R25 | R-bg-detach SSH background commands need full FD detach          |
| R26 | R-tracing-rigueur strace+blktrace+ftrace, not dmesg-grep         |
| R27 | R-anchor-exact Python patch via byte-for-byte count==1           |
| R28 | R-empirical-state read disk before patch, no session-memory      |

---

## 7. Trajectoire mainline (8 phases)

[Phase 0] Cadrage formalised                    CLOSED 2026-05-01
[Phase 1] Format v5 minimal RFC-able baseline   PENDING (active prep)
[Phase 2] fsck.beamfs MVP                       PENDING
[Phase 3] Multiblock read_folio (VFS)           PENDING
[Phase 4] Stage 4 close + paper v3 draft        PENDING
[Phase 5] DKMS + Yocto layer                    PENDING
[Phase 6] Build user base (anti-NAK)            PENDING
[Phase 7] Documentation/filesystems mainline    PENDING
[Phase 8] RFC mainline + review cycle           PENDING


Estimated total : 405h. beamfs-bench saved ~85h regression budget.

---

## 8. Quick navigation cheatsheet

- New to project ? -> read this file, then `context-recadrage.md` (R0-R37)
- Current state ? → `context/STATUS.md`
- Deferred work ? → `context/TODO.md`
- Threat model ? → `Documentation/threat-model.md`
- On-disk format ? → `Documentation/format-v4.md` (current) + `format-v5.md` (future)
- Bench harness ? → `~/git/beamfs-bench/README.md`
- Yocto image ? → `yocto-beamfs/recipes-core/images/hpc-arm64-research-beamfs.bb`
- Plan ? → `Documentation/roadmap.md`

End of mindmap.
