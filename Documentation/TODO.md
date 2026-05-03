# beamfs TODO list

> **CLASSIFICATION INTERNAL - NEVER PUSH TO PUBLIC GITHUB**
>
> This file is versioned only on `roastercode/beamfs-devel` PRIVATE
> branch. When `roastercode/beamfs` PUBLIC v3 is published (cf
> `context/context-recadrage.md` section 0), the `public-v3-staging`
> filter MUST exclude `Documentation/TODO.md` from the cherry-pick /
> rebase set. This file contains internal threat-model analysis,
> forensic findings, pre-publication reasoning, and strategic
> decisions queued -- none of which is public-facing.

**Authoritative**: this file is the **single source of truth** for
outstanding work across the four beamfs repositories AND the session
opener (read top to bottom in 5 minutes for context). It supersedes
fragmentary status across `roadmap.md`, `known-limitations.md`, and
prior session handoffs.

**Last updated**: 2026-05-03

**Cross-repository scope**:

| Repo                                    | Branch                  | Visibility | Latest commit |
|-----------------------------------------|-------------------------|------------|---------------|
| `roastercode/beamfs-devel`              | `diag/double-free-block`| PRIVATE    | `b57e342` |
| `roastercode/beamfs` (vitrine, frozen)  | `main`                  | PUBLIC     | (frozen)      |
| `roastercode/yocto-beamfs`              | `diag/double-free-block`| PRIVATE    | `458817b` |
| `roastercode/beamfs-bench`              | `main`                  | PRIVATE    | `e6c7441` |
| `roastercode/radfi`                     | `main`                  | PRIVATE    | `1fb9aeb`     |

All 4 PRIVATE repos pushed and clean as of 2026-05-03 R19 validation
on the sub-1.B+C+D bundle (`beamfs-bench full --auto-confirm` exit 0,
cluster Phase 6 12/12 VERIFIED, multifs beamfs MOUNTED 3/3 with full
RS recovery under RadFI v0.1.3 attack at prob=1k/100k/1M).

---

## How to use this file (session opener)

Read sections 1 -> 6 in order at the start of every session :

1. **Schemas** (doc map + code map + mindmap)  -> orient the project mentally
2. **Empirical state**                         -> know what is shipped on disk
3. **Target architecture**                     -> know where we are heading
4. **Critical path to kernel.org RFC**         -> know the next milestone
5. **Strategic decisions queued**              -> know which forks need a call
6. **Documentation drift to repair**           -> quick wins (1-2h, doc-only)

Then jump to the catalog (sections 8-13) for item-level work, or to
the **Priority matrix** (section 14) for guided next-step selection.

---

## 1. Schemas (orientation)

### 1.1 Documentation map (post-rationalisation 2026-05-02)

```
                            beamfs/Documentation/
                                       │
            ┌──────────────────────────┼──────────────────────────────┐
            │                          │                              │
       NORMATIVE                   AUDITED                       OPERATIONAL
       (PUBLIC)                   (PUBLIC)                       (INTERNAL)
            │                          │                              │
   ┌────────┴─────────┐      ┌─────────┴──────────┐         ┌────────┴────────┐
   │                  │      │                    │         │                 │
threat-model.md    mainline-     known-          empirical-     TODO.md   context/
(EM threat model)  scope.md   limitations.md    state.md   (CRITICAL    (INTERNAL)
section 6 = 6.1..6.6  (RW+RS-FEC  (gap  vs       (live cluster   PATH +     │
constraints           position;   threat-       results 77 obs   item-by-   ├ 00-MINDMAP.md
                      capabilities model;       per mega run)   item        │  (R0-R30 map)
                      matrix)     drift list)                   tracker)    │
                                                                            ├ STATUS.md
                       │            │                    │                  │  (3 repos HEAD)
                       │            │                    │                  │
                       v            v                    v                  ├ context-recadrage.md
                  ┌────────────────────────────────────────┐                │  (R0-R30 contract)
                  │          DESIGN DOCS                    │                │
                  │  ┌──────────────────┐                   │                ├ INLINE-MULTIBLOCK-DESIGN.md
                  │  │ design.md        │ (current arch)    │                │  (substep 4-10 plan)
                  │  │ format-v4.md     │ (CURRENT format,  │                │
                  │  │                    naming inherited  │                └ patches/, archive/
                  │  │                    from FTRFS;       │
                  │  │                    actually beamfs   │
                  │  │                    v1 fresh)         │
                  │  │ format-v5-design.│ (TARGET, skeleton)│
                  │  │ md               │                   │
                  │  │ fsck.beamfs.md   │ (Phase 2 design)  │
                  │  │ system-          │ (positioning vs   │
                  │  │ architecture.md  │  dm-verity, etc.) │
                  │  │ architecture-    │ (current cluster) │
                  │  │ current.md       │                   │
                  │  └──────────────────┘                   │
                  └─────────────────────────────────────────┘
                                       │
                                       v
                  ┌─────────────────────────────────────────┐
                  │             ROADMAP                      │
                  │  roadmap.md (Stage 1.5 → 5 + Phase 0-8)  │
                  │  testing.md (validation chain)           │
                  └─────────────────────────────────────────┘


Cross-reference rules (enforced by document conventions)
─────────────────────────────────────────────────────────

  threat-model.md sec 6 ──> known-limitations.md  (gap items reference 6.N)
                       ──> roadmap.md             (stage closure refs constraint)
                       ──> mainline-scope.md      (sec 2 reproduces sec 6 list)

  mainline-scope.md   ──> format-v5-design.md    (sec 3 = profiles spec)
                      ──> roadmap.md Phase 0-8   (sec 5 = trajectory)

  TODO.md (this file) ──> roadmap.md             (Phase mapping table)
                      ──> known-limitations.md   (gap-N items)
                      ──> All design docs        (See also section)


Public vs Private classification
─────────────────────────────────

  PUBLIC (Documentation/) :
    threat-model.md, mainline-scope.md, roadmap.md, format-v5-design.md,
    format-v4.md, design.md, known-limitations.md, fsck.beamfs.md,
    system-architecture.md, architecture-current.md, empirical-state.md,
    testing.md

  INTERNAL (Documentation/, gated by header) :
    TODO.md (this file ; explicit "NEVER PUSH PUBLIC" header)

  INTERNAL (context/, gitignored except whitelist) :
    00-MINDMAP.md, STATUS.md, context-recadrage.md, INLINE-MULTIBLOCK-DESIGN.md
```

**Post-audit 2026-05-02 changes :**

- `context/TODO.md` (322 lines, obsolete duplicate) : **REMOVED**
- `context/INLINE-MULTIBLOCK-DESIGN.md` (1064 lines, substeps 4-10
  closed) : **ARCHIVED** to
  `context/archive/INLINE-MULTIBLOCK-DESIGN-2026-05-02-closed.md`
  (audit trail preserved, no longer active editing)
- `format-v4.md` : naming clarification header added
  (`BEAMFS_VERSION_V1 = 1` is the kernel version constant ; "v4"
  refers to the superblock layout family inherited from FTRFS lineage)
- `roadmap.md` : Stage 3 status `ACTIVE` -> `CLOSED 2026-05-02` ;
  Item 4 Shannon entropy `PENDING` -> `CLOSED 2026-05-02`
- `known-limitations.md` : table line 6.4 Shannon entropy "Not
  implemented" -> "Implemented in stage 3 item 4 (closed 2026-05-02)"
- `beamfs/README.md` : R17 tagline corrected
- `yocto-beamfs/README.md`, `radfi/README.md` : R17 tagline edits
  pending in WT, deferred to next session of those repos

### 1.2 Code map (4 projects)

### beamfs (kernel module + userspace tooling repo)

```
~/git/beamfs/
│
├── Kernel module sources (~5000 LOC C, out-of-tree)
│   ├── beamfs.h            ── on-disk structs (SB, inode, RS event 40-byte) + feature flags
│   │                          + version constants (BEAMFS_VERSION_V1 = 1, fresh format)
│   │                          + 11 INCOMPAT / 4 RO_COMPAT / 3 COMPAT bits reserved
│   │
│   ├── super.c             ── mount/umount, SB CRC32+RS recovery, mount-time feature
│   │                          flag enforcement (ext4-pattern), beamfs_log_rs_event
│   │                          with Shannon entropy
│   │
│   ├── edac.c              ── RS(255,239) encode/decode + position list output for
│   │                          forensic entropy ; LUT-based Shannon entropy Q16.16
│   │                          (gen_entropy_lut.py auto-generated)
│   │
│   ├── inode.c             ── beamfs_iget + RS-protected inode write/read + i_nlink mgmt
│   │
│   ├── alloc.c             ── block + inode allocation + 2-layer canary defense
│   │                          (S_IMMUTABLE + silent skip) against double-free
│   │
│   ├── dir.c               ── beamfs_dir_operations (readdir)
│   ├── namei.c             ── beamfs_dir_inode_operations (lookup/create/unlink)
│   │                          + dirent slot reuse fix (4b-dirent CLOSED)
│   │
│   ├── file.c              ── beamfs_file_operations (legacy non-INLINE path)
│   │
│   └── file_inline.c       ── INLINE multi-block path : write_begin/write_end (substep 4),
│                              writepages (substep 5), truncate via setattr (substep 6),
│                              mmap via generic_file_mmap (substep 8), tri-block folio
│                              fix (substep 10) — Stage 3 metadata hardening
│
├── tools/                  ── userspace
│   ├── checkpatch-precommit.sh  (lint pre-commit, no baseline yet)
│   ├── decode_raf_journal.py    (RS journal forensic parser)
│   ├── gen_entropy_lut.py       (regenerate edac.c LUT)
│   └── fsck.beamfs/             (PLACEHOLDER, empty — Phase 2 deliverable)
│
├── Documentation/          ── 13 .md files (see schema 1)
│
├── context/                ── INTERNAL (gitignored except whitelist)
│   ├── 00-MINDMAP.md       ── R0-R30 map + repo classification
│   ├── STATUS.md           ── 3-repo HEAD tracker per session
│   ├── context-recadrage.md  ── R0-R30 operational contract
│   └── INLINE-MULTIBLOCK-DESIGN.md  ── substep 4-10 design
│
└── papers/
    ├── 2026-04-beamfs-v1/  ── Zenodo published
    ├── 2026-04-beamfs-v2/  ── Zenodo published (paper.pdf, CC-BY-4.0)
    ├── 2026-04-beamfs-v3-findings/  ── lab notebook only (no paper.tex yet)
    └── 2026-04-radfi-v1/   ── companion radfi paper PDF


Lockstep contract with yocto-beamfs
────────────────────────────────────
  beamfs/{beamfs.h,super.c,file_inline.c,file.c,inode.c,dir.c,namei.c,
          edac.c,alloc.c,...}
                ↕  byte-identical (sha256-validated by R19 phase 0.2)
  yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/{same files}
```

### yocto-beamfs (PRIVATE Yocto layer for cluster image)

```
~/git/yocto-beamfs/                     PRIVATE, branch diag/double-free-block
│
├── conf/                              ── layer + distro conf
├── recipes-kernel/
│   ├── linux/
│   │   ├── linux-mainline_7.0.3.bb    ── kernel 7.0.3 stable
│   │   ├── BEAMFS-arm64.cfg           ── kernel config aarch64
│   │   └── files/multifs.cfg          ── multifs additional kernel options
│   │
│   ├── beamfs/
│   │   ├── beamfs-module_0.1.0.bb     ── beamfs.ko (lockstep mirror)
│   │   ├── mkfs-beamfs_0.1.0.bb       ── mkfs.beamfs userspace tool
│   │   └── files/beamfs-0.1.0/        ── source mirror (12 files, lockstep)
│   │
│   ├── radfi/
│   │   └── radfi-module_0.1.2.bb      ── radfi.ko companion EM injector
│   │
│   └── lttng/                         ── (kernel tracing, not active in R19)
│
├── recipes-beamfs/
│   └── beamfsd/                       ── EM Resilience Journal daemon
│
├── recipes-hpc/                       ── slurm, munge, pmix (HPC stack ready,
│                                         not activated — func-9)
│   ├── slurm/                         ── slurm 25.11.4
│   ├── munge/                         ── munge 0.5.18
│   ├── pmix/                          ── pmix 5.0.3
│   └── libevent/
│
├── recipes-core/
│   └── images/
│       └── hpc-arm64-research-beamfs.bb  ── ★ CANONICAL R23 image recipe
│                                            (sole active image, deploys 4 VMs)
│
├── recipes-devtools/                  ── cmake, elfutils, llvm, qemu, rust, etc.
├── recipes-extended/                  ── bash, libtirpc, unzip
├── recipes-graphics/                  ── glslang
├── recipes-support/                   ── gdbm, gmp
│
├── beamfs-bench/                      ── (subtree, deployed binary build)
├── bin/                               ── helper scripts (lockstep-validate, etc.)
│
└── Documentation/
    ├── beamfs-integration.md          ── (out-of-snapshot in R19 export)
    └── runs/                          ── 1.6 GB of R19 manifests + tarballs
                                          (excluded from snapshots, audit-grade
                                          forensic archive only)
```

### beamfs-bench (PRIVATE Rust harness)

```
~/git/beamfs-bench/                     PRIVATE, branch main, ~5600 LOC Rust + 1029 shell
│
├── Cargo.toml + Cargo.lock            ── crate metadata
│
└── src/
    ├── main.rs            (426 LOC)   ── CLI entry, sub-command dispatch
    │
    ├── pipeline.rs        (372 LOC)   ── R19 8-phase pipeline (lifecycle, bootstrap,
    │                                     multifs, cluster, analyse, forensics, GPG)
    │
    ├── lifecycle.rs       (318 LOC)   ── VM destroy + start + SSH-wait parallel
    │                                     + R-isolation enforcement
    │
    ├── bootstrap.rs       (132 LOC)   ── insmod + mkfs.beamfs + mount /data on 4 nodes
    │
    ├── cluster.rs         (276 LOC)   ── cluster_setup_all/attack_all/verify_all
    │                                     orchestration over SSH
    │
    ├── ssh.rs             (136 LOC)   ── SSH transport (exec, exec_lenient, scp_to)
    │
    ├── multifs.rs         (408 LOC)   ── 5-FS head-to-head on USB pass-through
    │                                     + worker.sh embedded via include_str!
    │
    ├── synthesis.rs       (463 LOC)   ── verdict derivation : multifs ladder
    │                                     RS_RECOVERED|RS_PASSTHROUGH|RS_FAILED|
    │                                     FS_PANIC|CORRUPTED_DATA + 9 cargo tests
    │                                     (cluster ladder NOT YET WIRED)
    │
    ├── analyse.rs         (421 LOC)   ── Quick / Standard / Full scope orchestration
    │
    ├── forensics.rs       (371 LOC)   ── VM-side capture (dmesg, ftrace, perf, lsmod)
    ├── forensics_host.rs  (292 LOC)   ── HOST-side capture + bpftrace opt-in
    │                                     (block_rq_complete + sched_switch counts)
    │
    ├── bitrot.rs          (262 LOC)   ── offline bit-rot scenarios (5 FS x 4 cells)
    ├── metadata.rs        (330 LOC)   ── deterministic block targeting (5 FS x 4 cells)
    ├── crash.rs           (329 LOC)   ── virsh destroy mid-write + remount verify
    ├── fsck.rs            (229 LOC)   ── offline FS check (beamfs returns NOT_IMPLEMENTED)
    │
    ├── devices.rs         (364 LOC)   ── USB device discovery + R12 anti-NAK prompt
    ├── mega.rs            (463 LOC)   ── consolidated 10-phase mega run
    │
    └── worker.sh         (1029 LOC)   ── deployed via SCP to /tmp/beamfs-bench-worker.sh
                                          on every VM ; 17 case actions :
                                          setup/attack/verify (multifs)
                                          cluster_setup/attack/verify
                                          bootstrap_data
                                          bitrot_setup/inject/verify
                                          metadata_setup/inject/verify
                                          crash_setup/start_writer/verify
                                          fsck_check
```

### radfi (PRIVATE EM fault injection kernel module)

```
~/git/radfi/                            PRIVATE, tag v0.1.2-palier3-validated
│
├── README.md              (3 KB)      ── ★ DRIFT : says "PRE-ALPHA, no code yet"
│                                          but code is shipped and runtime-validated
│
├── Documentation/
│   ├── README.md
│   ├── design-notes.md
│   └── EMPIRICAL-RESULTS.md
│
├── papers/
│   └── v1/                            ── 13 .tex sections + paper.tex + Makefile
│       │                                 + refs.bib + paper.pdf (Zenodo published)
│       └── (sections 00-abstract, 01-introduction, ..., 12-references)
│
└── src/radfi-0.1.2/       (500 LOC)   ── kernel module sources
    ├── radfi.h            (66 LOC)    ── debugfs interface : target_dev, target_block,
    │                                     probability, enabled, hook_blk, call_count,
    │                                     flip_count
    │                                     (target_block_range NOT YET implemented)
    ├── radfi_main.c       (142 LOC)   ── init/exit + debugfs scaffolding
    ├── radfi_inject.c     (86 LOC)    ── probabilistic flip selection (no FIEMAP yet)
    ├── radfi_hooks_blk.c  (125 LOC)   ── block layer hook (bio path)
    │                                     ★ DIRTY working tree (uncommitted edits)
    ├── radfi_hooks_fs.c   (81 LOC)    ── FS layer hook (deferred)
    └── Makefile
```

### beamfs-overlay (PRIVATE Gentoo overlay, not in snapshot scope)

```
/var/db/repos/beamfs-overlay/           PRIVATE, branch main, single commit 011bf20
│
└── sys-fs/
    └── beamfs-bench/
        └── beamfs-bench-9999.ebuild   ── installs /usr/bin/beamfs-bench from
                                          file:///home/aurelien/git/beamfs-bench
                                          + sudoers entry for libvirt group
```

### Cross-repo lockstep
```
   beamfs (PRIVATE devel, PUBLIC origin frozen)
        │
        │  byte-identical 12 sources
        │  validated by R19 phase 0.2 SHA256
        ↓
   yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/
        │
        ↓
   bitbake hpc-arm64-research-beamfs
        │
        ↓
   /var/lib/libvirt/images/hpc-arm64/beamfs-{master,compute0[1-3]}.img
        │
        ↓
   4 VMs running insmod beamfs.ko + radfi.ko
        │
        ↓
   beamfs-bench full --auto-confirm  (R19 critical pre-push validation)
```

### 1.3 Architecture mindmap (target v5 mainline RFC-able)

```
                          ┌──────────────────────────────────────────┐
                          │   beamfs - resilient filesystem          │
                          │   "RW Linux FS with native RS-FEC,       │
                          │    fills mainline gap (dm-fec is RO)"    │
                          └────────────────┬─────────────────────────┘
                                           │
            ┌──────────────────┬───────────┼─────────────┬────────────────────┐
            │                  │           │             │                    │
            v                  v           v             v                    v
    ┌──────────────┐   ┌─────────────┐  ┌─────────┐  ┌──────────┐    ┌───────────────┐
    │ THREAT MODEL │   │  ON-DISK    │  │  CODE   │  │ USER     │    │  MAINLINE     │
    │  (universal) │   │  FORMAT v5  │  │  PATH   │  │ TOOLING  │    │  TRAJECTORY   │
    └──────┬───────┘   └──────┬──────┘  └────┬────┘  └─────┬────┘    └───────┬───────┘
           │                  │              │             │                 │
           v                  v              v             v                 v
   ┌────────────────┐ ┌───────────────┐ ┌──────────┐ ┌────────────┐  ┌──────────────┐
   │ SEU/MBU        │ │ SINGLE FORMAT │ │ READ     │ │mkfs.beamfs │  │ Phase 0 DONE │
   │ NAND/DRAM age  │ │ + flags       │ │ → RS     │ │  --profile=│  │              │
   │ IEMI attacks   │ │   (ext4-like) │ │   decode │ │  embedded  │  │ Phase 1 (50h)│
   │ Voltage glitch │ │               │ │ → CRC32  │ │  server    │  │  v5.0 minimal│
   │ Rowhammer      │ │ 11 INCOMPAT   │ │   verify │ │  dax       │  │  RFC-able    │
   │ Cosmic rays    │ │  4 RO_COMPAT  │ │ → folio  │ │            │  │              │
   │                │ │  3 COMPAT     │ │   pop    │ │fsck.beamfs │  │ Phase 2 (30h)│
   └────────────────┘ │  bits         │ │          │ │  --check   │  │  fsck MVP    │
            │         │               │ │ WRITE    │ │  --repair  │  │              │
            v         │ Mount-time    │ │ → CRC32  │ │            │  │ Phase 3 (12h)│
   addressed by ──→   │ enforcement   │ │   compute│ │tune.beamfs │  │  multiblock  │
                      │ (live, Stage 3│ │ → RS     │ │  --upgrade-│  │  read_folio  │
                      │  closed)      │ │   encode │ │  format    │  │              │
                      │               │ │ → block  │ │            │  │ Phase 4 (100h)│
                      │ s_version=V1  │ │   write  │ │            │  │  Stage 4     │
                      │ (fresh)       │ │          │ │            │  │  + paper v3  │
                      └───────────────┘ │ JOURNAL  │ └────────────┘  │              │
                              │         │ → RS     │                 │ Phase 5 (25h)│
                              v         │   event  │                 │  DKMS+meta   │
                   ┌─────────────────┐  │   logged │                 │              │
                   │ PROFILES        │  │   with   │                 │ Phase 6 (75h)│
                   │                 │  │   Shannon│                 │  user base   │
                   │ embedded        │  │   entropy│                 │  3-5 deploy  │
                   │ (RFC v5.0)      │  │   Q16.16 │                 │              │
                   │ - 32K..100GB    │  │          │                 │ Phase 7 (50h)│
                   │ - 4K..1GB       │  └──────────┘                 │  rst+chkpatch│
                   │ - HW-bound      │                               │  +xfstests   │
                   │ - 0 flags       │                               │              │
                   │                 │                               │ Phase 8 (60h)│
                   │ server          │                               │  RFC mail    │
                   │ - 1GB..16EB     │                               │  + review    │
                   │ - 4K..16EB      │                               │              │
                   │ - 50-300µs      │                               │ TOTAL: 405h  │
                   │ - EXTENTS+64BIT │                               └──────────────┘
                   │   +BLOCK_GROUPS │                                       │
                   │   +JOURNAL      │                                       v
                   │                 │                            ┌──────────────────┐
                   │ dax             │                            │  KERNEL.ORG RFC  │
                   │ - 64GB..100TB   │                            │  ┌────────────┐  │
                   │ - 4K..16EB      │                            │  │ cover ltr  │  │
                   │ - 1-10µs        │                            │  │ + patches  │  │
                   │ - embedded+DAX  │                            │  │   atomic   │  │
                   └─────────────────┘                            │  │ + paper v3 │  │
                                                                  │  │   DOI cite │  │
                                                                  │  │ + 3-5 user │  │
                                                                  │  │   citations│  │
                                                                  │  └────────────┘  │
                                                                  │  linux-fsdevel   │
                                                                  └──────────────────┘



──────────────────────────────────────────────────────────────────────────────────
                          OUT-OF-SCOPE (defended in cover letter)
──────────────────────────────────────────────────────────────────────────────────

  ❌ RAID native           → delegated to dm-raid / mdraid
  ❌ Snapshots CoW         → delegated to LVM thin / btrfs subvol overlay
  ❌ Encryption native     → delegated to dm-crypt
  ❌ Compression           → orthogonal, possibly v6+
  ❌ Network FS            → local block device only
  ❌ MTD/UBI native        → block device only in v5
  ❌ Migration in-place ext4→beamfs  → too complex, copy migration only



──────────────────────────────────────────────────────────────────────────────────
              ANTI-NAK DOCTRINE (lessons from FTRFS rejection 2025)
──────────────────────────────────────────────────────────────────────────────────

  Rule 1 :  RFC submission = embedded profile only (~5-8 k LoC)
            Anti-NAK firewall, opposite of bcachefs all-at-once approach
            Trajectory : f2fs / exfat (modest entry, growth via patches)

  Rule 2 :  3-5 public deployments cited in cover letter (Phase 6 deliverable)
            FTRFS was rejected primarily on "show me the users"

  Rule 3 :  Paper v3 published Zenodo with DOI, cited in cover letter
            Empirical anchor for the resilience claim ; without it,
            same FTRFS trajectory ("toy fs", NAK)

  Rule 4 :  Out-of-scope explicit and defended (above list)
            Reviewers will probe each capability not listed ;
            preemptive scope statement neutralizes 80% of objections

  Rule 5 :  checkpatch.pl --strict zero (Phase 7 DoD)
            Style-NAK fatal (bcachefs-style ejection)



──────────────────────────────────────────────────────────────────────────────────
                          COMPONENT FLOW (runtime, R19 validated)
──────────────────────────────────────────────────────────────────────────────────

    ┌──────────────────────┐
    │ Application userspace│  ← read()/write()/mmap()/truncate() syscalls
    └──────────┬───────────┘
               │
               v
    ┌──────────────────────┐
    │ VFS (Linux kernel)   │  ← inode_operations, file_operations
    └──────────┬───────────┘
               │
               v
    ┌──────────────────────────────────────────────────────────┐
    │ beamfs.ko                                                 │
    │  ┌─────────────────────────────────────────────────────┐ │
    │  │ super.c : mount, feature flag enforcement,           │ │
    │  │           SB CRC32+RS recovery, journal log         │ │
    │  └─────────────────────────────────────────────────────┘ │
    │  ┌─────────────────────────────────────────────────────┐ │
    │  │ inode.c + namei.c + dir.c : VFS inode/dir ops       │ │
    │  └─────────────────────────────────────────────────────┘ │
    │  ┌─────────────────────────────────────────────────────┐ │
    │  │ file_inline.c : INLINE multi-block read/write/mmap   │ │
    │  │   (substep 4-10, 1249 LOC)                          │ │
    │  └─────────────────────────────────────────────────────┘ │
    │  ┌─────────────────────────────────────────────────────┐ │
    │  │ alloc.c : block + inode allocation + canary defense │ │
    │  └─────────────────────────────────────────────────────┘ │
    │  ┌─────────────────────────────────────────────────────┐ │
    │  │ edac.c : RS(255,239) encode/decode + entropy LUT     │ │
    │  └─────────────────┬───────────────────────────────────┘ │
    └────────────────────┼─────────────────────────────────────┘
                         │ depends on
                         v
    ┌──────────────────────┐
    │ lib/reed_solomon     │  ← mainline kernel
    └──────────┬───────────┘
               │
               v
    ┌──────────────────────┐
    │ Block layer (bio)    │
    └──────────┬───────────┘
               │
               v ← (validation only) radfi.ko hooks bio path here
    ┌──────────────────────┐
    │ Block device         │  ← /dev/vdb (cluster) or /dev/vdc..vdg (multifs USB)
    └──────────────────────┘



──────────────────────────────────────────────────────────────────────────────────
              EMPIRICAL VALIDATION (closed runtime — Stage 3 results)
──────────────────────────────────────────────────────────────────────────────────

   4-VM aarch64 cluster (libvirt + QEMU TCG, kernel 7.0.3) :
   ├ master    192.168.56.10    (orchestrator, never victim, R-isolation R21)
   ├ compute01 192.168.56.11    (FS-test holder : 5 USB pass-through victims)
   ├ compute02 192.168.56.12    (cluster member, beamfs on /data)
   └ compute03 192.168.56.13    (cluster member, beamfs on /data)

   beamfs-bench full (R19 canonical pre-push) results (mega 2026-05-01) :
   ├ multifs   : beamfs RECOVERED 3/3 (probs 1k/100k/1M) ✅
   ├ cluster   : beamfs RECOVERED 12/12 (4 nodes x 3 probs), DIFFS=0 ✅
   ├ bitrot    : 20 observations across 5 FS x 4 cells ✅
   ├ metadata  : 20 observations across 5 FS x 4 cells ✅
   ├ crash     : 4 R/W FSes remount mount_rc=0, stable_files_ok=5/5, no PANIC ✅
   ├ fsck      : ext4/ext3/btrfs rc=0 ; beamfs NOT_IMPLEMENTED (Phase 2 pending)
   └ dmesg     : 0 BUG, 0 Oops, 0 WARN ✅

   Empirical proof of RS-FEC functional correctness (Stage 3 metadata closed,
   Stage 4 data block protection still pending — func-3).
```

---

## 2. Empirical state (factual, code-verified at 2026-05-02)

This section enumerates capabilities that source inspection confirms
are live runtime, regardless of what auxiliary documents claim.

### 2.1 Kernel module (beamfs.ko)

**Filesystem core**
- `s_magic = 0x4245414D` (BEAM), `s_version = BEAMFS_VERSION_V1 = 1`
  (fresh BEAMFS format ; no migration from v2/v3/v4 FTRFS lineage)
- 4096-byte fixed block size
- 256-byte fixed inode size
- Direct (12) + indirect (1) + dindirect (1) + tindirect (1) addressing
  ; max single-file capacity 524 inline-blocks (~2 MiB) per current
  allocator
- `mkfs.beamfs` userspace (`yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/mkfs.beamfs.c`)
  emits `s_version = BEAMFS_VERSION_V1` consistent with kernel

**RS-FEC error correction (Reed-Solomon 255,239)**
- Bitmap protected by 16 RS sub-blocks (Stage 1.5 v2 closed)
- Superblock protected by 13 RS sub-blocks of 211 data bytes,
  with parity at offset 3888, total coverage 2709 bytes
  (Stage 3 item 2 closed)
- All inodes RS-protected unconditionally under
  `s_data_protection_scheme = INODE_UNIVERSAL` (Stage 3 item 1 closed)
- `beamfs_rs_decode` returns symbol count (>0) on success, 0 if no
  errors detected, negative errno if uncorrectable, plus optional
  `positions[]` output for forensic entropy estimation
- `beamfs_rs_decode_region` symmetric variant for multi-subblock regions

**Shannon entropy in RS journal (Stage 3 item 4)**
- 40-byte `struct beamfs_rs_event` with `re_symbol_count`,
  `re_entropy_q16_16`, `re_flags`, `re_reserved`, `re_crc32`,
  `re_pad` (`beamfs.h:212-222`)
- LUT-based entropy computation via
  `beamfs_rs_compute_entropy_q16_16` in `edac.c:110` (no FPU,
  no runtime division, deterministic in cycles)
- `BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID` set when n_positions >= 2
  (single-sample entries cleared)
- `beamfs_log_rs_event` (`super.c:301`) records each correction
  event with computed entropy in the on-disk journal of 64 entries
  (1536 bytes embedded in superblock)
- 40-byte struct sentinels enforced via `BUILD_BUG_ON` and
  `static_assert` at compile time

**Feature flag scaffolding (ext4-pattern)**
- 3 COMPAT bits reserved (`RS_JOURNAL_VERBOSE`, `LABEL_LONG`,
  `DIR_INDEX`)
- 4 RO_COMPAT bits reserved (`LARGE_FILE`, `HUGE_FILE`,
  `EXTRA_ISIZE`, `BTREE_DIR`)
- 11 INCOMPAT bits reserved (`EXTENTS`, `64BIT`, `BIGALLOC`,
  `BLOCK_GROUPS`, `BTREE_ALLOC`, `JOURNAL`, `DAX`, `RS_HEAVY`,
  `PER_INODE_RS`, `BG_RS_PARITY`, `LARGE_BLOCK`)
- Mount-time enforcement live (`super.c:570`):
  - INCOMPAT unknown bit → mount refused
  - RO_COMPAT unknown bit → forced read-only
  - COMPAT unknown bit → informational log
- `BEAMFS_FEAT_*_SUPP = 0` : **no flag is currently active runtime**
  ; the scaffolding is ready, the per-flag implementations are not

**INLINE multi-block read/write/mmap/truncate path**
- `file_inline.c` 1249 LOC : write_begin/write_end multi-block
  (substep 4), writepages multi-folio (substep 5), truncate via
  setattr (substep 6 / func-2 closed), substep 7 closed,
  mmap support via generic_file_mmap (substep 8 closed),
  tri-block folio coverage fix (substep 10 / func-13 closed)
- `beamfs_inline_inode_operations` registers `simple_getattr` +
  `beamfs_inline_setattr`
- `beamfs_inline_file_operations` uses `generic_file_mmap`,
  `filemap_splice_read`, `generic_file_fsync`

**Defense-in-depth canary (substep 10)**
- 2-layer protection against double-free of reserved blocks
- Layer 1 : `S_IMMUTABLE` flag on reserved inodes blocks unlink/setattr
- Layer 2 : `beamfs_free_block` / `beamfs_free_inode_num` silent skip
  + early return when called on reserved blocks/inodes (covers Layer 1
  bypass via memory corruption)
- `dump_stack()` after `pr_warn` on legitimate path triggers, for
  forensic visibility (`alloc.c`)

**Dirent slot reuse fix (Stage 3 item 4b-dirent, closed 2026-04-26)**
- `beamfs_del_dirent` no longer zeros `d_rec_len`
- readdir + lookup advance unconditionally by
  `sizeof(struct beamfs_dir_entry)` ; free slot identified by
  `d_ino == 0`
- Static invariant `inv5_dirent_no_break_on_zero` as pre-commit guard

### 2.2 Userspace tooling

| Tool                       | State                          | Source                                                  |
|----------------------------|--------------------------------|---------------------------------------------------------|
| `mkfs.beamfs`              | implemented                    | yocto-beamfs/recipes-kernel/beamfs/files/.../mkfs.beamfs.c |
| `beamfsd`                  | implemented                    | yocto-beamfs/recipes-beamfs/beamfsd/                    |
| `inject_raf`               | implemented (legacy RAF acronym ; gap-1 rename pending) | beamfsd-bundled |
| `decode_raf_journal.py`    | implemented                    | beamfs/tools/decode_raf_journal.py                      |
| `gen_entropy_lut.py`       | implemented                    | beamfs/tools/gen_entropy_lut.py                         |
| `checkpatch-precommit.sh`  | implemented (no baseline run archived) | beamfs/tools/checkpatch-precommit.sh             |
| `fsck.beamfs`              | **not implemented (placeholder dir empty)** | beamfs/tools/fsck.beamfs/ (empty)         |
| `tune.beamfs`              | not implemented                | n/a                                                     |

### 2.3 RadFI companion

- 500 LOC C across `radfi.h`, `radfi_main.c`, `radfi_inject.c`,
  `radfi_hooks_blk.c`, `radfi_hooks_fs.c`
- Tag `v0.1.2-palier3-validated` (validated against beamfs Stage 3)
- debugfs interface : `target_dev`, `target_block`, `probability`,
  `enabled`, `hook_blk`, `call_count`, `flip_count`
- **Targeting** : `target_block=0` is broadcast across the device ;
  no range or list interface yet (radfi improvement: targeting
  precision pending, TODO line 730)
- Paper v1 PDF compiled (`papers/2026-04-radfi-v1/aurelien-desbrieres-radfi-v1-20260429.pdf`),
  Zenodo DOI 10.5281/zenodo.19885777
- README.md says "PRE-ALPHA, no code yet" : **out of date**, code is
  shipped and runtime-validated

### 2.4 beamfs-bench harness

- Rust binary `/usr/bin/beamfs-bench` 0.3.0
- Sub-commands : `full` (canonical R19), `multifs`, `analyse`,
  `bitrot`, `metadata`, `crash`, `fsck`, `mega`
- Worker shell `src/worker.sh` (1029 LOC, embedded via
  `include_str!` in multifs.rs) deployed to VMs via SCP, drives
  17 case actions
- Verdict derivation in Rust (`synthesis.rs` 463 LOC) : multifs
  scope produces `RS_RECOVERED|RS_PASSTHROUGH|RS_FAILED|FS_PANIC|
  CORRUPTED_DATA` ladder
- 9 cargo unit tests cover the multifs decision matrix
- Host-side forensic capture (`forensics_host.rs` 292 LOC) with
  `--bpftrace` opt-in flag
- R19 EXIT 0 reproducible 4 times in session 2026-05-02
- **Cluster scope verdict derivation : not wired** (factual records
  emitted, ladder not applied ; TODO line 871, 2-3h follow-up)

### 2.5 4-VM aarch64 cluster (libvirt + QEMU TCG)

- master (192.168.56.10), compute01..03 (.11/.12/.13)
- compute01 holds 5 USB pass-through victims (vdc..vdg : ext4,
  ext3, btrfs, squashfs, beamfs)
- All 4 VMs run kernel 7.0.3 + beamfs.ko + radfi.ko
- libvirt isolation enforced by `lifecycle.rs::assert_isolation_architecture()`
  in beamfs-bench Phase 0
- SSH `hpcadmin@<IP>` with `~/.ssh/hpclab_admin`

### 2.6 Empirical validation results (last full mega run 2026-05-01)

- multifs : beamfs RECOVERED 3/3 (probs 1k/100k/1M)
- cluster : beamfs RECOVERED 12/12 (4 nodes x 3 probs), DIFFS=0
- bitrot : 20 observations across 5 FS x 4 cells
- metadata : 20 observations across 5 FS x 4 cells (A1 SB, A2 bitmap,
  A3 inode, A4 saturation x3)
- crash : all 4 R/W FSes (ext4, ext3, btrfs, beamfs) remount with
  `mount_rc=0` and `stable_files_ok=5/5`, no DMESG_PANIC
- fsck : ext4/ext3/btrfs return rc=0 ; beamfs returns
  NOT_IMPLEMENTED
- 0 BUG, 0 Oops, 0 WARN in dmesg across all R19 runs

---


---

## 3. Target architecture (single source of truth on intent)

### 4.1 Position

beamfs is a read-write Linux filesystem with native inline RS-FEC
correction on the read path. It fills a mainline gap : RS-FEC is
currently only available via dm-fec, which is read-only and tied
to dm-verity. No mainline RW filesystem performs RS-FEC correction
on read-write storage.

The threat model addressed is universal : any Linux storage subject
to bit-flips of hardware (SEU/MBU, NAND/DRAM aging), environmental
(cosmic rays, IEMI), or adversarial (rowhammer, voltage glitch)
origin. The space deployment case is one specific instance ; the
datacenter and embedded cases are equally addressed.

### 4.2 Target on-disk format (v5 - per format-v5-design.md)

Single on-disk format parameterised by feature flags, exposing 3
mkfs profiles :

| Profile  | Volume range | File range | Latency | Active flags                          |
|----------|--------------|------------|---------|---------------------------------------|
| embedded | 32 KB - 100 GB | 4 KB - 1 GB | hardware-bound | none (RFC v5.0 minimal baseline) |
| server   | 1 GB - 16 EB | 4 KB - 16 EB | 50-300 µs | EXTENTS + 64BIT + BLOCK_GROUPS + JOURNAL |
| dax      | 64 GB - 100 TB | 4 KB - 16 EB | 1-10 µs | embedded flags + DAX                |

Migration : copy migration via mkfs + rsync (always supported) ;
in-place via `tune.beamfs --upgrade-format` for compatible flag
upgrades. Migration from ext4/btrfs/xfs requires copy.

### 4.3 RFC trajectory (per mainline-scope.md + roadmap.md Phase 0-8)

- **RFC v5.0 = embedded profile only** (~5-8 k LoC, anti-NAK firewall)
- Patch series 15-25 commits, atomic, rebased linear
- Subsequent patches activate flags one by one (f2fs/exfat
  trajectory, opposite of bcachefs all-at-once ejection)
- Cover letter cites paper v3 Zenodo DOI + 3-5 public deployments
  (anti-FTRFS-NAK doctrine)

### 4.4 Out of scope (explicit, defended in cover letter)

- RAID native (delegated to dm-raid/mdraid)
- Snapshots CoW (delegated to LVM thin / btrfs subvol overlay)
- Encryption native (delegated to dm-crypt)
- Compression (orthogonal, possibly v6+)
- Network FS (local only)
- MTD/UBI native (block device only in v5)

---


---

## 4. Critical path to kernel.org RFC (Phases 0-8)

This is the consolidated RAF (reste à faire) ordered by phase
dependency, with effort estimates from `roadmap.md`. Phase
preconditions are strict (Phase N+1 cannot start before Phase N DoD
passes).

### 4.2 Phase 1 - Format v5.0 minimal RFC-able (50h)

**Sub-1.A status** : CLOSED 2026-05-03 (commit `c44fc96` beamfs +
`03bc009` yocto-beamfs lockstep). `BEAMFS_VERSION_V5 = 5` declared
in `beamfs.h` alongside `BEAMFS_VERSION_V1 = 1`. Symbol available
for sub-1.B-1.F to reference. v1 still mountable at this point.

**Sub-1.B status** : CLOSED 2026-05-03 (no-code closure). The three
`BEAMFS_FEAT_(COMPAT|INCOMPAT|RO_COMPAT)_SUPP` masks were already
declared in `beamfs.h` (lines 370-372) with value `0ULL`, which is
exactly the v5.0 minimal RFC-able policy: no feature bit supported,
all 18 bits (3 COMPAT + 4 RO_COMPAT + 11 INCOMPAT) reserved for
future evolution (v5.1+). Mount-time enforcement at super.c:570-574
already references these masks. No code change required; sub-1.B
closed by documentation in this TODO entry.

**Sub-1.C status** : CLOSED 2026-05-03 (commit `1f56e9b` yocto-beamfs).
`mkfs.beamfs --profile=embedded` long-option added via `getopt_long`.
When `--profile=embedded` is passed, mkfs writes `s_version=5`;
otherwise the default behaviour was preserved at V1 (intentional
gate before sub-1.D). Runtime-validated host-side: gcc clean,
`s_version=5` verified at struct offset 56 with od.

**Sub-1.D status** : CLOSED 2026-05-03 (commit `83708da` beamfs +
`458817b` yocto-beamfs lockstep). Two atomic changes:
  - kernel `BEAMFS_VERSION_CURRENT` switched V1 -> V5; mount of
    pre-v5 images now fails with "unsupported on-disk version 1
    (this kernel requires v5)".
  - mkfs.beamfs `format_version` default bumped V1 -> V5 in lockstep,
    so all newly-created images are v5 by default;
    `--profile=embedded` becomes idempotent (still valid, adds the
    `[embedded]` info-line tag for explicit traceability).

R19 cycle on the bundle (sub-1.A+B+C+D): `beamfs-bench full
--auto-confirm` exit 0, cluster Phase 6 12/12 VERIFIED, Phase 5
multifs beamfs MOUNTED 3/3 with HASH_PRE==HASH_POST under RadFI
v0.1.3 attack at prob=1k/100k/1M (FLIP_DELTA up to 8 with full RS
recovery). beamfs.ko sha256 changed from `0c0bbe42...` to
`e955b536...`, confirming the V5 binary is what's loaded in-VM.

**Sub-1.E** : pending -- finalise `Documentation/format-v5.md`
sections 4-12 (currently TBD/TODO from Phase 0). Reference plan in
`Documentation/mainline-scope.md` section 5.

**Sub-1.F** : pending -- xfstests + checkpatch baseline pass on the
v5 mount path. Reference plan in roadmap.md Phase 1 final gate.

**DoD** : `beamfs.h` v5 + `mkfs.beamfs` userspace, format mountable
R/W on device.

What's already there :
- v5 feature flag bit allocations declared in `beamfs.h`
- Mount-time enforcement of unknown INCOMPAT/RO_COMPAT/COMPAT bits
- ext4-pattern feature flag detection live

What's missing :
- `s_version` bump to 5 (currently V1)
- format-v5.md complete (currently skeleton, sections 4-12 marked TBD)
- Per-flag implementation : `EXTENTS`, `64BIT`, `BLOCK_GROUPS`,
  `JOURNAL`, `DAX`, `BIGALLOC`, `BTREE_ALLOC`, etc. — none active
  in the RFC submission ; the embedded profile has 0 flags active
- mkfs.beamfs `--profile=embedded` (default) explicitly emits v5 SB
- Migration script v1 → v5 (`tune.beamfs --upgrade-format`) — for
  internal lab volumes, not deployment users

**TODO ID** : `func-12` (PER_INODE_RS feature flag is one specific
sub-item, not the whole Phase 1)

### 4.3 Phase 2 - fsck.beamfs MVP (30h)

**DoD** : `fsck.beamfs --check-only` passes 5 passes (SB, bitmap,
inode walk, bitmap rebuild, RS journal validation), exit codes per
fsck convention.

What's already there :
- `tools/fsck.beamfs/` directory placeholder
- `Documentation/fsck.beamfs.md` design document with implementation
  plan in 8 sub-phases (Phase 0 DONE, Phases 1-8 PENDING)
- Test D integration design in `beamfs-bench fsck`

What's missing :
- All 8 implementation sub-phases (skeleton + 5 passes + manpage +
  bench integration)
- `fsck.beamfs.8` manpage in mandoc format

**TODO ID** : `func-6`

### 4.4 Phase 3 - Multiblock read_folio (12h)

**DoD** : 15 boundary tests pass, fsstress 1h no corruption.

What's already there :
- Substeps 4 (write_begin), 5 (writepages), 6 (truncate / func-2),
  7 (closed), 8 (mmap), 10 (tri-block folio fix / func-13) all
  closed and validated
- Manifest 20260502T133223Z confirms substep 9 R19 EXIT 0
- 18/18 frontier scan N=1..50 OK after func-13 fix

What's missing :
- 15 boundary tests as a formal regression set in beamfs-bench
  (currently the frontier scan exists ad-hoc, not packaged in a
  cargo test or scope)
- fsstress 1h run never executed on beamfs ; required for DoD
- Per-`beamfs-bench evolution: INLINE frontier scan methodology`
  TODO entry, this is the path to DoD closure

**TODO ID** : `beamfs-bench evolution: INLINE frontier scan methodology`

### 4.5 Phase 4 - Stage 4 close + paper v3 draft (100h)

**DoD** : Paper v3 draft + Zenodo upload + arXiv preprint.

What's already there :
- `papers/2026-04-beamfs-v3-findings/SCIENTIFIC-FINDINGS-2026-04-30.md`
  : 16 KB raw findings note (lab notebook)
- LuaLaTeX + gnuplot stack validated on spartian-1
- paper v2 already published Zenodo (CC-BY-4.0)
- paper v1 RadFI companion published Zenodo

What's missing :
- Stage 4 data block protection : choice between scheme 2
  (UNIVERSAL_INLINE), scheme 3 (UNIVERSAL_SHADOW), scheme 4
  (UNIVERSAL_EXTENT) — design decision then implementation (2-4
  weeks)
- Paper v3 LaTeX tree (`papers/2026-04-beamfs-v3/`)
- Empirical validation post-Stage-4 (paper v3 needs the post-Stage-4
  numbers, not the current Stage 3 numbers)
- arXiv preprint upload
- Zenodo upload + DOI mint
- Cross-citations updated (paper v3 references RadFI v1, v2 paper)

**TODO IDs** : `func-3` (Stage 4 data block protection),
`upstream-7` (paper v3 publication)

### 4.6 Phase 5 - DKMS + Yocto layer (25h)

**DoD** : `dkms install` works on Debian, Ubuntu, Fedora, Gentoo ;
Yocto recipe ready for upstream meta-beamfs.

What's already there :
- yocto-beamfs internal layer (private, lab use)
- beamfs-overlay Gentoo overlay (private, dev use)
- `mkfs.beamfs` userspace builds via cargo + Makefile

What's missing :
- DKMS configuration (`packaging/dkms.conf`)
- `packaging/debian/` (debian/control, debian/rules)
- `packaging/rpm/` (.spec)
- Public meta-beamfs Yocto layer (separate from yocto-beamfs lab
  layer)
- Distribution-side packaging review and per-distro testing

### 4.7 Phase 6 - Build user base (75h)

**DoD** : 3-5 public deployments cited, technical blog post,
linux-fsdevel low-volume presence.

What's already there :
- Project foundations : 4 published papers (FTRFS v1, FTRFS v2,
  beamfs v1, beamfs v2) on Zenodo
- Phoronix coverage of FTRFS RFC v3 (lineage, not directly beamfs)

What's missing :
- 0 public beamfs deployments today
- Blog (technical writeup) on beamfs.org or equivalent
- Conference submission (FOSDEM, Kernel Recipes, EuroSys)
- Identifiable user base : DKMS package adopters, Yocto layer
  adopters, academic lab deployments, industrial deployments
- Each deployment must be public and citable (testimonial,
  technical writeup, or conference talk)

**TODO ID** : `upstream-8`

### 4.8 Phase 7 - Documentation/filesystems mainline + checkpatch zero (50h)

**DoD** :
- `Documentation/filesystems/beamfs.rst` Sphinx-ready
- `MAINTAINERS` entry
- `checkpatch.pl --strict` : 0 errors, 0 warnings on every .c/.h
- `fs/beamfs/` restructure (mainline kernel paths, not out-of-tree)
- selftests under `tools/testing/selftests/beamfs/`
- xfstests subset PASS (generic/{001,002,010,098,257} minimum)

What's already there :
- `Documentation/` (Markdown, project-internal)
- `Documentation/process/coding-assistants.rst` not yet present
- `tools/checkpatch-precommit.sh` script exists, no baseline run
- xfstests not run on beamfs (only generic Stage 3 R19 validation)

What's missing :
- `.md` → `.rst` transformation of `mainline-scope.md`,
  `format-v5-design.md`, `threat-model.md`, `known-limitations.md`,
  with adaptation to kernel doc tone
- `Documentation/process/coding-assistants.rst` policy doc (AI
  tooling disclosure required by linux-fsdevel)
- `MAINTAINERS` entry under `F: fs/beamfs/`, `F: Documentation/filesystems/beamfs.rst`
- Checkpatch baseline run, then iterative cleanup until zero
- Out-of-tree → in-tree restructure (`fs/beamfs/` per kernel layout)
- xfstests subset configuration (`local.config`, exclusion list,
  PASS report archived)
- selftests under `tools/testing/selftests/beamfs/` for
  format-specific edge cases

**TODO IDs** : `upstream-1` (xfstests), `upstream-3` (checkpatch
baseline), `upstream-4` (Documentation/filesystems entry)

### 4.9 Phase 8 - RFC mainline + review cycle (60h)

**DoD** : Patch series merged OR explicit NAK with corrective
actions for v5.1.

What's already there :
- nothing (Phase 8 is gated by all previous phases)

What's missing :
- Cover letter [PATCH RFC 0/N] (~2-3 pages)
- Patch series 15-25 commits, atomic, rebased linear, all GPG-signed,
  all `Signed-off-by` correct
- `git send-email` config validated for `aurelien@hackers.camp`
- linux-fsdevel mailing list subscription confirmed at
  `majordomo@vger.kernel.org`
- DKIM/SPF on hackers.camp validated against vger.kernel.org
- Test mail self-loop validates the full chain
- After mail : 3-6 months reviewer cycle, integrating feedback,
  resending v1, v2, ... until merge or explicit NAK

**TODO IDs** : `upstream-2` (reviewer feedback integration, post-mail),
`upstream-5` (cover letter), `upstream-6` (send-email + subscription)

### 4.10 Cumulative effort

| Phase | Hours | Cumulative | Status                  |
|-------|-------|------------|-------------------------|
| 0     | 3     | 3          | DONE 2026-04-30         |
| 1     | 50    | 53         | scaffolding done, flags pending |
| 2     | 30    | 83         | design done, code 0%    |
| 3     | 12    | 95         | substeps 4-10 done, formal DoD pending |
| 4     | 100   | 195        | Stage 4 + paper v3, 0%  |
| 5     | 25    | 220        | DKMS + meta-beamfs, 0%  |
| 6     | 75    | 295        | user base, 0%           |
| 7     | 50    | 345        | rst + checkpatch + xfstests, 0% |
| 8     | 60    | 405        | RFC mail + review, 0%   |

**Realistic** : 405 focused-work hours, **dominated by Phase 4
(100h paper v3) + Phase 6 (75h user base) + Phase 8 (60h review)**
which together account for 235h ≈ 58% of the budget.

The roadmap precondition graph is :

    Phase 0 (done)
       └→ Phase 1 ───┬→ Phase 2
                    ├→ Phase 3
                    ├→ Phase 4 (also needs 2, 3)
                    └→ Phase 5
                                └→ Phase 6
                                          └→ Phase 7 (also needs 1-3)
                                                    └→ Phase 8 (needs all)

The longest path = 0 → 1 → 4 → 6 → 7 → 8 ≈ 240h (all serial).
With concurrent execution where preconditions allow, calendar can
collapse to ~3-4 months sprint at 8h/day, or ~8 months at sustainable
pace 2-3h/day.

---


---

## 5. Strategic decisions queued

The following are not technical blockers but strategic forks that
will redirect ~50-100h of work each. They cannot be deferred
indefinitely without risk of rework.

### 5.1 Stage 4 data block protection scheme

Three documented options (`design.md` + `threat-model.md`) :
- **Scheme 2 UNIVERSAL_INLINE** : RS parity inline within each data
  block. Lower latency, smaller useful capacity per block. Already
  has empirical validation footprint (multifs scope tests it
  partially via INLINE multi-block).
- **Scheme 3 UNIVERSAL_SHADOW** : RS parity in dedicated out-of-band
  region. Higher latency for parity I/O, full data capacity per block.
- **Scheme 4 UNIVERSAL_EXTENT** : RS parity as filesystem attribute
  (xattr-style). Most flexible, most complex.

Decision impacts Phase 4 effort estimate (currently aggregate 100h
includes paper writing, but actual implementation can swing by 1-2
weeks).

### 5.2 RadFI mainline submission positioning

Three options :
- Out-of-tree permanent (lab tool only, distributed via beamfs-bench)
- Co-submission with beamfs (bundled RFC, single review cycle)
- Separate RFC track (independent companion to `lib/fault-inject.c`,
  `dm-flakey`, `fail_make_request`)

Decision impacts Phase 8 cover letter strategy.

### 5.3 Block size policy v5

**Status** : DECIDED 2026-05-03 (decision already locked in Phase 0
via `Documentation/mainline-scope.md` section 4.3, this entry
consolidates). Strict 4096-byte block size for v5.0; `BIGALLOC`
feature flag (bit 2, declared in `beamfs.h`) handles cluster_size
> 4 KiB. `LARGE_BLOCK` (bit 10) reserved but not activable in v5.0
(would multiply review surface vs page-cache pitfalls; deferred
to v5.1+). No further work required for sub-1.C `mkfs.beamfs
--profile=embedded`: profile writes block_size=4096 unconditionally.

`mainline-scope.md` declares 4096 fixed. But `BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK`
flag bit (10) is reserved for `block_size > PAGE_SIZE`.
Decision : keep 4096 strict for v5.0 RFC, defer LARGE_BLOCK to v5.x ?
Or activate LARGE_BLOCK from v5.0 with x86_64 + aarch64 page size
auto-detection ? Affects mkfs.beamfs CLI design.

### 5.4 Migration v1 → v5 scope

`format-v5-design.md` section 11 documents two paths but does not
choose. For Phase 1 Phase 5 transition, one of these is required.
- copy migration (always works, safe, simple)
- in-place upgrade (risky, complex tune.beamfs userspace tool)

For lab volumes only, copy migration is sufficient. For production
adopters (Phase 6 user base), in-place may be a hard ask.

---


---

## 6. Non-targets and project hygiene (explicit out-of-scope)

For honest record-keeping, the following items mentioned in some
session memories are explicitly **not part of beamfs target
architecture** :

- **Bpfs/distributed FS** : no, beamfs is local block-device only
  (mainline-scope.md section 6)
- **MTD/UBI native** : no in v5 (mainline-scope.md section 6)
- **PQC metadata authentication** : roadmap.md "post-merge long-term
  vision", post-v5
- **Encryption native** : delegated to dm-crypt (not in scope)
- **Compression** : orthogonal, possibly v6+
- **Snapshots CoW** : delegated to LVM thin / btrfs subvol
- **RAID native** : delegated to dm-raid/mdraid
- **Bpifrance/SASU/financing structure** : explicitly out of scope
  by user decision (recadrage Phase 0 acquis)
- **Migration in-place ext4 → beamfs** : declined for v5,
  reconsidered v6+ if demand

---


---

## 7. Documentation rationalisation (audit 2026-05-02 - applied)

Audit of all 41 .md files across 4 repos (beamfs / beamfs-bench /
yocto-beamfs / radfi, 11833 lines total). Eight rationalisation
actions identified and applied in this commit. The audit also
detected the long-standing drift items previously listed here ;
those drifts are now repaired by the same actions.

### 7.1 Doublons / obsolescence supprimes

- [x] **DELETE** `beamfs/context/TODO.md` (322 lignes obsoletes,
      doublon de `Documentation/TODO.md` au format pre-restructure
      avec items TODO 1-7 dont la majorite DONE).

      Items utiles rapatries dans la presente section :
      - TODO 1 lowercase normalization (350+950 occurrences) ->
        section 7.4 Tier 3 ticket
      - TODO 7 manifest provenance / SBOM / xfstests / checkpatch
        deja couverts par upstream-1, upstream-3, upstream-4

- [x] **ARCHIVE** `beamfs/context/INLINE-MULTIBLOCK-DESIGN.md`
      (1064 lignes, substeps 4-10 tous CLOSED) deplace vers
      `context/archive/INLINE-MULTIBLOCK-DESIGN-2026-05-02-closed.md`.
      Audit trail historique preserve, n'evolue plus comme document
      vivant.

### 7.2 R17 tagline violations corrigees

- [x] `beamfs/README.md` L1 : "beamfs - Beam-Resilient Filesystem"
      -> "beamfs - resilient filesystem"

- [ ] `yocto-beamfs/README.md` L4 : "BEAMFS v1 (Beam-Resilient
      Filesystem)" -> "beamfs v1 (beamfs - resilient filesystem)"
      [edit pending, commit deferred to next yocto-beamfs session]

- [ ] `radfi/README.md` L7-9 : "PRE-ALPHA - paper draft only. No
      code yet." factuellement faux (500 LOC C shipped, tag
      v0.1.2-palier3-validated, paper Zenodo DOI 10.5281/zenodo.19885777).
      [edit pending, commit deferred to next radfi session ; combinable
      avec les 2 fichiers source dirty actuels]

### 7.3 Drift status interne corrige (commit current)

- [x] `format-v4.md` header : naming clarification ajoutee
      (`BEAMFS_VERSION_V1 = 1` is the kernel constant ; "v4" is the
      superblock layout family name from FTRFS lineage)

- [x] `roadmap.md` L25 status table : Stage 3 `ACTIVE` -> `CLOSED 2026-05-02`
      (items 1, 2, 3, 4, 4a, 4b-dirent all closed ; only release
      ceremony tag `v0.3.0-metadata-hardening` remains)

- [x] `roadmap.md` L353 : Item 4 Shannon entropy `PENDING` -> `CLOSED 2026-05-02`
      (struct 40-byte, LUT entropy, journal recording all live runtime)

- [x] `known-limitations.md` L49 (table line 6.4) : "Not implemented"
      -> "Implemented in stage 3 item 4 (closed 2026-05-02)" with
      full description of struct layout and runtime path

### 7.4 Tickets de rationalisation residuels (Tier 3, post-RFC)

- [ ] `doc-1` : lowercase normalization audit
      Origin : ex-`context/TODO.md` TODO 1 (rapatrie ici).
      Effort : ~2-3h. 350 occurrences `BEAMFS` dans .md/.txt et
      ~950 dans .c/.h strings/comments cross-repo. Convention
      kernel.org : lowercase `beamfs` en prose et dmesg, MAJ
      preserve pour macros C, magic ASCII "BEAM", tex/bib papers.
      Decoupage en 3 PRs (md/txt batch, dmesg strings audit,
      kernel comments audit). Pas critique pour RFC submission ;
      `checkpatch --strict` (upstream-3) ne flag pas la casse en
      prose.

- [ ] `doc-2` : `design.md` reposition
      Effort : ~30 min. Header "Block Layout (v3)" + spec v3
      coexiste avec `format-v4.md` qui s'autodeclare autoritaire
      pour v4. Soit renommer `format-history.md`, soit reduire a
      un index pointant vers `format-v4.md`. Pas urgent (les deux
      docs cohabitent proprement, format-v4 est explicite sur la
      precedence).

### 7.5 Compte final post-audit

| Etat                  | Avant audit | Apres audit |
|-----------------------|-------------|-------------|
| Total .md across 4 repos | 41        | 39 (-2)     |
| Total lignes prose    | 11833       | ~9800 (-2033 : delete + archive moves) |
| Doublons actifs       | 1 (context/TODO.md) | 0 |
| Drift documente       | 6 items     | 0 (1 delete + 5 fixes appliques) |
| README en violation R17 | 3         | 1 (beamfs OK ; yocto + radfi pending) |

---

### 7.6 R17 cleanup post-audit (2026-05-03 - applied)

Audit R17 (canonical tagline `beamfs - resilient filesystem`,
all lowercase, hyphen-minus) revealed FTRFS-lineage descriptors
still present in code-side metadata that the 2026-05-02 audit had
missed (focused on Markdown only). Fixes applied lockstep beamfs
<-> yocto-beamfs in commit `d02b1ec` + `516e26d`:

- `Makefile` descriptor: `BEAMFS - Fault-Tolerant Radiation-Robust
  Filesystem` -> `beamfs - resilient filesystem`
- `Kconfig`: header comment, tristate label
  (`BEAMFS Beam Electromagnetic File System (EM resilience)` ->
  `beamfs - resilient filesystem`), 2 prose lines in help text
- 8 SPDX headers in `.c` files (alloc, dir, edac, file, file_inline,
  inode, namei, super): `BEAMFS - <X>` -> `beamfs - <X>`
- 6 prose comments in `.c`/`.h` (file_inline:550,569; namei:523;
  super:704; beamfs.h:257,294,296)
- `recipes-kernel/radfi/radfi-module_*.bb` DESCRIPTION: `BEAMFS v1`
  -> `beamfs v1` (commit `81c57ef` yocto-beamfs)
- `radfi/README.md`: scientific positioning section, one
  occurrence (commit `1fb9aeb` radfi standalone)

Untouched (legitimate): C macro identifiers (`BEAMFS_*`,
`CONFIG_BEAMFS_FS`, `BEAMFS_MAGIC`); academic citations of FTRFS
(Fuchs, Langer, Trinitis 2015) in Kconfig help text and READMEs;
audit log entries in `Documentation/archive/` and prior TODO sections.
Lockstep R9 verified post-patch: 11/11 source files identical
beamfs <-> yocto-beamfs.

### 7.7 R31 added to context-recadrage.md (2026-05-03 - applied)

Operational rule R31 added to recadrage (commit `749ff0e`)
following Phase 0.7 identity check failure during sub-1.A push
session. R31 enforces that any commit touching kernel files
(`.c`/`.h`) in lockstep beamfs <-> yocto-beamfs must verify the
changes are reflected in the `beamfs.ko` loaded by the cluster
VMs BEFORE invoking `beamfs-bench full` and BEFORE pushing.

Founding incident: QEMU was actively writing to
`beamfs-master.ext2` during a redeploy `cp`, causing rootfs to
diverge from canonical. Fix applied: `virsh destroy` + `cp -f`
canonical .ext2 + relaunch. Subsequently codified as a
preventive check inside `beamfs-bench` itself (cf section 13
evolution closure).

## 8. Documentation/code coherence gaps (4 items)

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

## 9. Functional gaps

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

## 10. Upstream submission status

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

## 11. radfi improvements

### radfi improvement: targeting precision (range/list/file-aware)

**Status** : PARTIAL 2026-05-03. First sub-item closed: `target_block`
filter extended to the bio-layer hook (commit `1fb9aeb` radfi v0.1.3).
Previously `target_block` applied only on the fs hook (sb_bread /
submit_bh path); now also enforced on the blk hook
(submit_bio_noacct path), unifying targeting semantics across both
transport layers. Validated R19 with `RADFI_VERSION 0.1.3` loaded
in-VM, `FLIP_DELTA=2/8` at prob=100k/1M with HASH_POST stable on
beamfs (RS_PASSTHROUGH 3/3).

Remaining sub-items (range filter, list filter, file-aware
targeting): OPEN.

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

## 12. beamfs improvements

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

## 13. beamfs-bench improvements

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

### beamfs-bench evolution: R31 invariant + tarball forensic enrichment [CLOSED 2026-05-03]

Three commits applied to `beamfs-bench` following the Phase 0.7
identity check failure incident (cf section 7.7 R31).

**Commit `62a0640` -- fix(pipeline): R31 verify .ext2 byte-identity** :
  `redeploy_4_vms()` now adds `sync(1)` after `destroy_all_vms`
  to force libvirt/QEMU FD release; computes canonical .ext2
  sha256 once before the loop; after each `cp` + `chown`,
  syncs again and verifies deployed .ext2 sha256 matches
  canonical (mismatch -> `bail!` with diagnostic pointing to
  R31). Catches stale FDs from out-of-pipeline VMs at Phase 0.5
  instead of surfacing only at Phase 0.7.

**Commit `5f8ad6f` -- feat(forensics): tarball R31 audit trail** :
  `forensics_host.rs::pre_capture_host` extended with 9 new
  artefacts per run: `git-{repo}.txt` x 4 (HEAD signatures,
  status, branch, remotes); `bitbake-provenance.log`;
  `vm-rootfs-format.log` (qemu-img info + sha256 canonical vs 4
  VMs); `identity-{vm}.txt` x 4 (in-VM beamfs.ko sha256 + lsmod);
  `vm-state-{vm}.log` x 4 (df, mount, lsblk, ip a, cmdline,
  os-release); `modinfo-{vm}.log` x 4 (full modinfo for beamfs,
  reed_solomon, radfi). `analyse.rs::make_tarball` extended:
  copies `manifest-<TS>.json{,.asc}` into `host/`; writes
  `MANIFEST.sha256` at run_dir root (post-extraction integrity).

**Commit `e6c7441` -- fix(forensics): R31 capture bugs** :
  Three bugs caught at first R19 retry of the enriched tarball:
  (A) known_hosts desynchronization on identity-{vm}.txt after
  VM rebuild (R13 piege) -> add `ssh-keygen -R <ip>` before each
  ssh_capture + `StrictHostKeyChecking=accept-new`. (B) multi-line
  shell script quoting via Rust Debug-format collapsed newlines
  -> base64-encode remote_cmd, decode + pipe to `bash -s`
  remote-side. (C) manifest .json copy never landed because
  filter included .json.asc; sort + last() picked .asc -> tighten
  filter to `ends_with('.json')`.

## 14. Priority matrix

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

## 15. See also

- `Documentation/architecture-current.md` : current runtime architecture
- `Documentation/threat-model.md` : EM threat model + v4 journal nomenclature
- `Documentation/system-architecture.md` : positioning vs dm-verity, squashfs, VxWorks, PikeOS
- `Documentation/roadmap.md` : Stage 3 / Stage 4 / Stage 5 plan ; Phase 0-8 mainline preparation roadmap (DoD per phase)
- `Documentation/mainline-scope.md` : v5 position, threat model, capabilities matrix, out-of-scope
- `Documentation/format-v5-design.md` : v5 on-disk format design (skeleton, populated phase by phase)
- `Documentation/fsck.beamfs.md` : fsck.beamfs design (Phase 2 deliverable)
- `Documentation/known-limitations.md` : current scheme limitations
- `context/context-recadrage.md` : R0-R30 operational rules + Phase 0 strategic acquis (PRIVATE)
