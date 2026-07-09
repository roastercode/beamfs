# beamfs - TODO / RAF
*Updated: 2026-07-08 (post system update + §3.10 closure session)*

---

## CLOSED this session (2026-07-08)

### §3.10 evict_inode BUG_ON (CLOSED)
Root cause: buffer_heads attached to `inode->i_data.i_private_list`
by `mark_buffer_dirty_inode()` were not detached before `clear_inode()`.
Fix: `invalidate_inode_buffers(inode)` before `clear_inode()` in
`beamfs_evict_inode()` (commit 11c844f). Defense-in-depth: I_FREEING
guard in `beamfs_write_bitmap_block()` (f964b27), NULL owner in
eviction-path `beamfs_free_block()` calls (f146cbd). Validated R19
under RadFI injection, tainted=4096 (OOT-only) on all 4 nodes.

### System update (Gentoo spartian-1)
- World update 619 packages, kernel hôte 6.18.18 → 7.1.3 (Alder Lake)
- CFLAGS `-march=native` → `-march=alderlake` via resolve-march-native
- Python 3.14 as system default (python3.13 for bitbake, pending upstream fix)
- beamfs-bench ebuild sudoers doublon fix (9999 + 24 versioned ebuilds)

### Yocto HPC stack bumps
- libevent 2.1.12 → 2.1.13 (security: integer overflow, OOB read, HTTP smuggling)
- pmix 5.0.3 → 5.0.10 (bugfix series, slurm hang fix)
- linux-mainline 7.0.3 → 7.0.9 (6 stable point releases)

---

## OPEN - beamfs kernel

### §3.11 RS(255,239) silent miscorrection on data blocks (TRIAGED)
Format v6 + per-block CRC32 deferred. Under high-density EM injection
(probability=100000 ppm), RS can "correct" a data block to a valid
but wrong codeword. Requires per-block CRC32 as second integrity layer.

### §3.12 emufi disabled on compute01 (TRIAGED)
emufi module present on disk but not loaded/functional on compute01.
Root cause not isolated.

### Theorem v2.2b (conjecture)
Saturation observability on data blocks - split from v2.2a (proven)
in commit 66c8eb8. Conditional on format v6 per-block CRC32.

---

## OPEN - beamfs-bench

### TUI dashboard (ratatui)
Real-time multi-panel terminal UI for `beamfs-bench full`:
- VM state panel (virsh list, tainted status per node)
- RadFI injection counters and progress per FS/probability
- dmesg tail per node (filtered: beamfs/radfi/BUG/Oops)
- bitbake build progress (task N/M, current recipe)
- SSH worker activity (which node, which phase)
Motivated by repeated blind spots during long bench runs in this
session. Reference: htop-style layout, ratatui Rust crate.

### Runtime verbosity + structured logging
beamfs-bench currently outputs minimal progress on stdout during
attack/verify phases. Needed: structured log file (JSON or NDJSON)
with per-event timestamps, per-node status, injection parameters,
and phase transitions. Enables post-mortem analysis without
re-running the full bench.

### Fix locate_repo_root() heuristic
`multifs.rs::walk_up_for_repo()` searches for `Documentation` +
`bin` + `beamfs-bench` directories walking upward from cwd - only
matches when launched from within `yocto-beamfs/`. Should search
sibling directories or accept an env var `BEAMFS_YOCTO_ROOT`.
Currently worked around by launching from `~/git/yocto-beamfs/`.

---

## OPEN - Gentoo system (spartian-1)

### depclean pending
51 obsolete packages (old gcc, llvm-21, python-3.13, rust-bin-1.95,
gentoo-sources-6.18.18). Safe to run after confirming no runtime
dependency on python-3.13 beyond bitbake.

### ocaml/coccinelle chain broken
ocaml-gettext-0.4.2.1 fails to compile against ocaml 5.4 (AST
breakage). Masked in package.mask. coccinelle removed from world
set (depends on camlp4 which requires ocaml-gettext). Revisit when
Gentoo upstream fixes ocaml-gettext.

### guestfs-tools excluded
guestfs-tools-1.54.0 hardcodes `libguestfs[ocaml]` unconditionally
in its ebuild. Excluded from world update. Not needed for beamfs
workflow.

### python3.13 as bitbake default
bitbake (poky styhead) crashes under python3.14 due to
PicklingError in multiprocessing (forkserver start method change).
python3.13 set as eselect default until upstream fix in bitbake.

### CONFIG_DEBUG_INFO_BTF absent
bpftrace/bpftool functional but emit BTF warnings at build time.
Low priority - not blocking any workflow.

### Microcode amd-uc.img in grub.cfg
Cosmetic: grub references AMD microcode but CPU is Intel i7-12700F.
No functional impact.

---

## OPEN - Kernel security / bug bounty

### RDMA/rtrs-srv integer underflow
Patch submitted to linux-rdma@vger.kernel.org (June 2026).
Acked-by from Haris Iqbal received. Jason Gunthorpe (maintainer)
involved. Status: awaiting merge into rdma-next. Google buganizer
issue closed as Infeasible (not Android scope).
