# beamfs Known Limitations

**Status**: factual record of unresolved limitations in the current
codebase (HEAD at the time of this document's last revision).
**Audience**: downstream integrators, kernel reviewers, future
contributors, certification auditors.
**Last updated**: 2026-04-26.

---

## 1. Purpose and scope

This document records what is **known to be incomplete, suboptimal,
or absent** in the current beamfs implementation, with the intent of
giving an honest baseline for anyone reading the source tree.

It is the operational counterpart to two normative documents:

- `Documentation/threat-model.md` defines what the architecture
  must achieve. This document records the gap between that target
  and the current implementation.
- `Documentation/roadmap.md` defines what is planned next. This
  document does not duplicate the roadmap; it states the present.

Items listed here are **not commitments to fix on a particular
schedule**. Some will be addressed; some may be reclassified as
"won't fix" with documented rationale; some may be superseded by
architectural changes that make them moot. The honest enumeration
matters more than the resolution timeline.

When an item is resolved, it is removed from this document and
appears in the relevant commit message and release notes.

---

## 2. Architectural limitations relative to the threat model

Each item below references the corresponding constraint in
`threat-model.md` section 6. The current implementation does not
yet satisfy these constraints. Resolution is the principal subject
of subsequent development stages.

| Threat model constraint | Current implementation state |
|--------------------------|-------------------------------|
| 6.1 Universal data block protection (no opt-in) | Not met. RS FEC protects the on-disk allocation bitmap and, optionally, inodes flagged with `BEAMFS_INODE_FL_RS_ENABLED`. Since 2026-07-09 DATA_CSUM (format-v6) stores a per-data-block CRC32 and detects a substituted block (3.11), but it is opt-in (`mkfs.beamfs --data-csum`) and detects rather than corrects; the default path leaves data blocks unprotected. The constraint asks for no opt-in and for correction. |
| 6.2 Burst tolerance through stripe geometry | Not implemented. The bitmap uses 16 RS(255,239) sub-blocks packed within a single 4 KiB block, with no cross-block parity distribution. A burst exceeding 8 symbols within one 256-byte sub-block is uncorrectable in the current design. |
| 6.3 Unconditional inode RS protection | Implemented in stage 3 (v0.3.0+). All inodes are RS-protected unconditionally under `s_data_protection_scheme = INODE_UNIVERSAL`. The legacy `BEAMFS_INODE_FL_RS_ENABLED` flag is preserved in the bit definition for backward compatibility but is no longer functional. |
| 6.3 Superblock RS correction | Implemented in stage 3 item 2 (v0.3.0+). The superblock is protected by both CRC32 (detection) and RS(255,239) shortened over 8 sub-blocks of 211 data bytes (correction). On mount, a CRC32 mismatch triggers `beamfs_rs_decode_region()` over the staging buffer, with parity at offset 3968 of the superblock block. Recovery succeeds on up to 8 byte errors per sub-block. The corrected superblock is persisted to disk on the next metadata mutation via `beamfs_dirty_super()`, which encodes RS parity then recomputes CRC32 in that order. See `Documentation/design.md`, sections "Superblock CRC32" and "Superblock RS FEC". |
| 6.4 Shannon entropy in RS journal | Implemented in stage 3 item 4 (closed 2026-05-02). Per-event Shannon entropy recorded as Q16.16 fixed-point via LUT-based computation in `edac.c::beamfs_rs_compute_entropy_q16_16` (no FPU, no runtime division). The `struct beamfs_rs_event` is 40 bytes (was 24): adds `re_symbol_count`, `re_entropy_q16_16`, `re_flags`, `re_reserved`, `re_crc32`, `re_pad`. The `BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID` flag is set when `n_positions >= 2`. Family A (Poisson) versus Family B (correlated burst) discrimination is now recorded inline at correction time, not inferential.|
| 6.5 Bounded auditable code size | Currently met. Total source approximately 2700 lines kernel-side; the 5000-line target leaves margin for the items above plus planned features. |
| 6.6 In-kernel, in-place, on-IO-path correction | Met for metadata and bitmap. The IO path uses iomap with a hook on the bitmap read path; the equivalent hook on the data block read path is part of the work to address constraint 6.1. |

---

## 3. Implementation correctness items

These are localized issues identified by code review or operation,
where the code does not crash or misbehave in observed runs but
where the behavior is either ambiguous, defensive coverage is
missing, or dead code remains in tree.

### 3.1 Dead or unused declarations in `beamfs.h` (RESOLVED stage 3)

`BEAMFS_INODE_RS_DATA` and `BEAMFS_INODE_RS_PAR` are now used by
`namei.c::beamfs_write_inode_raw` and `inode.c::beamfs_iget` to
compute and verify per-inode RS parity under the
`INODE_UNIVERSAL` scheme. `BEAMFS_INODE_FL_RS_ENABLED` is
retained as a deprecated bit definition, documented in
`beamfs.h`, for backward compatibility with v0.1.0 / v0.2.0
images that may have set it. New images do not set it.

### 3.2 Inode number leak in `beamfs_create` error path

In the namei.c `beamfs_create` path, an allocated inode number can
be leaked if a subsequent step fails (e.g., directory entry write
failure). The error unwinding does not currently call
`beamfs_free_inode_num` on all failure branches.

### 3.3 Inconsistent handling of `d_rec_len == 0` between `dir.c` and `namei.c`

`dir.c::beamfs_readdir` treats `d_rec_len == 0` as the end of valid
directory entries within a block. `namei.c` paths that scan
directories for free slots or for renames have a slightly different
convention. The two should be aligned, with the chosen convention
documented inline.

### 3.4 Missing bounds check in `mkfs.beamfs`

`mkfs.beamfs` does not currently verify
`total_blocks <= 30592 + data_start_blk` before formatting. The
on-disk bitmap, with 16 sub-blocks of 239 bytes each addressing
8 bits per byte, can address at most 30592 blocks. A larger device
will be silently formatted with a bitmap that under-represents the
addressable space.

### 3.5 Semantic mismatch in `beamfs_rs_decode` return convention (RESOLVED 2026-04-26)

`edac.c::beamfs_rs_decode` returns 0 on success (corrected or clean)
and `-EBADMSG` on uncorrectable failure. The number of symbols
corrected is not returned to the caller.

`alloc.c::beamfs_setup_bitmap` consumes the return value with the
condition `if (rc > 0)` to identify a "corrected sub-block" event,
which never matches under the current decoder semantics. As a
consequence, the Electromagnetic Resilience Journal currently does not log
bitmap corrections that did occur.

This is a behavioral defect: corrections are applied (the bitmap
data is restored), but the operational record is incomplete. Two
fixes are possible:

1. Modify `beamfs_rs_decode` to return the symbol count on success
   while keeping `-EBADMSG` for uncorrectable, and update all
   callers.
2. Have `beamfs_setup_bitmap` re-derive whether a correction occurred
   by comparing pre- and post-decode buffers.

Option (1) is preferred because it provides the symbol count
needed for the entropy estimate (constraint 6.4).

**Resolution (stage 3 item 3, 2026-04-26)**: Option (1) implemented.
`beamfs_rs_decode` now returns the corrected symbol count on success
(`> 0`), `0` if no errors were detected, or a negative `errno` on
uncorrectable. `beamfs_rs_decode_region` propagates the same convention
through the per-subblock `results[]` array. Existing callers were
updated: `inode.c::beamfs_iget` now passes the actual symbol count to
`beamfs_log_rs_event` (previously hardcoded to `0`), and the
`alloc.c::beamfs_setup_bitmap` `rc > 0` branch is now reachable as
intended. Logging of superblock RS recoveries to the journal remains
deferred to a later item.

This finding is recorded here for the first time; it was identified
during the architectural review that produced `threat-model.md`.

### 3.6 beamfs_crc32_sb declared but undefined (RESOLVED 2026-04-26)

From commit fd371f3 through commit b60ac1b (the v0.1.0-baseline tag),
beamfs.h declared the function `beamfs_crc32_sb` and `super.c` called
it from `beamfs_fill_super` to verify the on-disk superblock CRC32,
but no definition was ever committed. Out-of-tree builds against a
properly configured kernel module build environment failed at the
final link stage with an unresolved symbol error.

The bug went unnoticed because the host development workstation runs
a kernel whose beamfs.h dependencies prevent any out-of-tree build,
and because the runtime test path on Yocto historically used a .ko
built from a workspace where the function existed locally without
ever being committed.

Resolved in commit cdfe78b: the function is now defined in `edac.c`
with coverage matching `mkfs.beamfs.c::crc32_sb` byte-for-byte. A
follow-up commit (4ca1859) extended the coverage to the v3 layout.

Implication for `v0.1.0-baseline`: the tag points to a tree where
this build error is present. The Sigstore signature on the tarball
remains cryptographically valid; the affected behaviour is the
ability to build the module out-of-tree from that tag, not the
contents of the artefact.

### 3.7 lib/reed_solomon API call signature mismatch (RESOLVED 2026-04-26)

`beamfs_rs_encode` and `beamfs_rs_decode` in `edac.c` were calling
`encode_rs8` and `decode_rs8` with a `uint16_t syms[]` buffer.
The kernel API at `include/linux/rslib.h` takes `uint8_t *data`
on the data-buffer argument; the call produced incompatible
pointer-type errors at build time on linux-mainline 7.0:

  edac.c:85: error: passing argument 2 of 'encode_rs8' from
                    incompatible pointer type
  edac.c:115: error: passing argument 2 of 'decode_rs8' from
                    incompatible pointer type

Latent since commit 4a2198c (migrate RS FEC to lib/reed_solomon).
Resolved in commit 867a911: pass `data` directly to the kernel APIs
which already accept `uint8_t *`. The intermediate `uint16_t syms[]`
buffer was redundant; removing it also drops the post-decode
copy-back loop in the decode path, since `decode_rs8` corrects in
place.

This is a kernel-API drift issue: an earlier rslib.h convention may
have used `uint16_t *`. The current kernel-mainline 7.0 used by
this project requires the `uint8_t *` form.

### 3.8 d_type=DT_FIFO on hardlink creation (RESOLVED 2026-05-15)

`namei.c::beamfs_link` passed a literal `1` as the `file_type`
argument to `beamfs_add_dirent`. The dirent ABI uses the standard
`DT_*` enum where `1 == DT_FIFO`. Hardlinks were therefore recorded
on disk with the FIFO type, causing `getdents(2)` to return the
wrong `d_type` for every hardlink. Tools that trust `d_type` without
falling back to `stat(2)` (e.g. `find -type`, recursive shell
walkers, the busybox `ls -l` acceleration path) misclassified
hardlinks as named pipes. `stat(2)` returned the correct mode
because it reads `i_mode` from the on-disk inode, which is
unaffected; only the `d_type` byte stored in the dirent slot was
wrong.

**Resolution** (commit `6fd2d96`, 2026-05-15): derive the dirent
type from the source inode's `i_mode` via `fs_umode_to_dtype()`,
the kernel-provided `umode_t -> DT_*` mapping (`linux/fs.h`). A
hardlink to a regular file now records `DT_REG`, a hardlink to a
symlink records `DT_LNK`, and so on. Hardlinks to directories are
forbidden by VFS so the `S_IFDIR` case is not reachable. Prerequisite
for any first-boot validation of a beamfs-served Linux rootfs where
busybox or other tools relying on `d_type` are present.

### 3.9 RMW transit not serialised against concurrent writers (RESOLVED 2026-05-15)

The INLINE writeback path (scheme=2 UNIVERSAL_INLINE) RMW transit
on `bh->b_data` was not serialised against concurrent writers
on the same physical block. The per-inode `i_alloc_mutex` only
covered the indirect-tree lookup, not the subsequent `sb_bread` +
decode + splice + `rs_encode` + `mark_buffer_dirty` sequence. Two
writeback paths reaching the same `phys` (bdi flusher + `fsync`,
or distinct folios sharing an intermediate INLINE disk block in
the tri-block coverage case) interleaved their re-scatter and
`rs_encode` steps. The result was a corrupted on-disk codeword
while each folio's page-cache copy stayed uptodate: the canonical
hot-sha vs cold-sha mismatch symptom observed under multi-process
load on the dindirect addressing range.

**Resolution** (commit `157b2ea`, 2026-05-15): wrap the full RMW
transit in `lock_buffer(bh)` / `unlock_buffer(bh)`. The buffer_head
lock serialises per-`phys`-block, not per-inode, so parallel
writes on disjoint blocks remain parallel. Reads on the same `phys`
are also serialised against writers via the same lock, which fixes
the reader-vs-writer corruption symmetrically. `unlock_buffer` is
called before `sync_dirty_buffer` (which re-locks internally) to
avoid deadlock; between the unlock and the sync the buffer is
dirty and RS-encoded, so any concurrent reader sees a valid
codeword. `decode_block_into_buf` was refactored to accept a
pre-`bread` + pre-locked `bh` so the lock contract is explicit
in the signature and callers (`writeback_folio`, `zero_tail_block`,
`read_folio`) own the `bh` lifecycle.

---

### 3.10 Single-occurrence `double free of block N` warn at boot on rootfs=.beamfs (CLOSED 2026-07-08)

**Symptom.** Under certain rootfs state conditions (see below), booting
a VM with `root=/dev/vda rw rootfstype=beamfs` against the Yocto-produced
`beamfs-research-image` rootfs image emits one kernel warn at
`t ≈ 10s` post-mount:

```
beamfs: double free of block N
CPU: ... PID: ... Comm: rm Not tainted ...
Call trace:
  beamfs_free_block+0xfc/0x130
  beamfs_evict_inode+0x148/0x2a0
  evict+0xd8/0x238
  iput.part.0+0x134/0x240
  iput+0x1c/0x38
  filename_unlinkat+0x1a8/0x298
  __arm64_sys_unlinkat+0x4c/0x90
  ...
```

The warn fires at `t ≈ 10s` post-mount but is **non-systematic**
across boots and **state-dependent**: it appeared on three consecutive
boots immediately after an R19 cluster bench run (master=block 51153,
compute01/02/03=block 276098, deterministic per node across these
three reboots), then **disappeared** on subsequent boots after interim
activity (ftrace setup with `virsh destroy + virsh start` cycles,
create+rm runtime stress tests, additional reboot tests). On those
later boots, `Comm=rm` still frees a block at `t ≈ 10s` but the
block (e.g. 51152) is a legitimately allocated block and the canary
does not fire.

The trigger condition therefore appears to be specific rootfs state
left behind by some operation. Most likely candidates: (a) the R19
bench-driven SSH writes to the master rootfs (logs, history, transient
config files) creating an inode whose direct/indirect pointer slot
clashes with a freshly-allocated block in a later truncate path;
(b) RS-correction at mount of the rootfs bitmap if any vda block had
been flipped by a previous run (note: R19 attacks /dev/vdb only;
vda is not directly targeted, but the dmesg from boot 3 reported
4 corrected bitmap subblocks at mount, which is unexplained if only
vdb was attacked).

**Impact.** None on this boot path: the canary in `alloc.c::beamfs_free_block`
silently rejects the second free, the bitmap stays self-consistent, the
rootfs survives, mount remains beamfs, no panic, no Oops, witness files
in `/etc` persist across reboots. The R19 bench harness runs to exit 0
under EM injection (`beamfs-bench full --injector emufi`) without any
correlated regression.

**Risk.** If the same block is referenced by two distinct in-use
inodes and the kernel reuses the apparently-free block for a third
inode before the second one is unlinked, silent data corruption is
possible. To date, no such reuse has been observed; the rootfs hashes
remain stable across reboot cycles (`/lib/modules/7.0.3/modules.alias`
sha256 identical on the 4 nodes after 2+ reboot cycles).

**What is known about the cause.**

1. The on-disk image produced by `mkfs.beamfs --from-dir` is
   bitmap-coherent: an exhaustive offline audit (Python parser walking
   direct + indirect + dindirect + tindirect-L1, cross-checked against
   the RS-decoded on-disk bitmap) reports zero double allocations and
   zero referenced-but-free blocks. Mount-time bitmap initialization in
   `alloc.c` reads the on-disk bitmap faithfully into `s_block_bitmap`.

2. Runtime reproduction attempts fail: creating then unlinking small
   files (single 4 KiB), a tight loop of 100 small files, or a 10 MB
   file exercising the indirect path do **not** add new `double free`
   warns past the boot-time one. The free path in `super.c::beamfs_free_data_blocks`
   (direct + indirect only; dindirect/tindirect free is a separate
   gap, not exercised by rootfs files in this image) is correct in
   steady state for the paths that are exercised.

3. The warn originates from `Comm=rm` at `t ≈ 10s`, i.e. during the
   Yocto sysvinit run-level switch where scripts such as
   `populate-volatile.sh`, `bootmisc.sh`, `read-only-rootfs-hook.sh`
   create then immediately unlink transient files on the rootfs.

4. ftrace + kprobe runtime traces (via kernel cmdline
   `kprobe_event="p:fb beamfs_free_block blk=%x1;r:ab beamfs_alloc_block ret=%x0"`)
   show that during the `t < 12s` boot window only three calls to
   `beamfs_free_block` occur, all originating from `beamfs_inline_setattr`
   (truncate-down on `/etc` files by init scripts), plus the final
   `rm` from `beamfs_evict_inode`. **No calls to `beamfs_alloc_block`
   are recorded in that window**, so the corruption is not a fresh
   allocation collision; it pre-exists the boot.

5. A `virsh destroy` after `truncate-without-sync` (mimicking the
   R19 bench cleanup pattern) does **not** reproduce the warn on the
   following boot. The dirty inode from the in-memory truncate is
   discarded by destroy and the on-disk inode retains its original
   pointer/size, so no desync between bitmap and inode is created
   by destroy alone.

6. Therefore the corruption is most likely created by a **write-path
   or truncate-path operation that completed and synced cleanly**
   (so it survives across reboots) but produced an inconsistency
   between an inode's pointer slot and the bitmap state for the
   referenced block. The exact operation has not been isolated; the
   four bitmap corrections at mount on boot 3 (block 0 sub 1, block 1
   sub 10, block 2 sub 13, block 3 sub 3) suggest a single byte flip
   in an early bitmap block may be the proximate cause, but the
   provenance of that flip on /dev/vda (not the R19 target) is open.

**Reproduction path for future investigation.** When the warn is
observed on a fresh boot, the offending block number can be captured
in real time by adding the following to the kernel cmdline in
`libvirt-defs/beamfs-master.xml` (then `virsh define + destroy + start`):

```
kprobe_event="p:fb beamfs_free_block blk=%x1;r:ab beamfs_alloc_block ret=%x0" trace_buf_size=8M tp_printk
```

then post-boot:

```
sudo bash -c "echo 1 > /sys/kernel/debug/tracing/events/kprobes/fb/enable"
sudo bash -c "echo 1 > /sys/kernel/debug/tracing/events/kprobes/ab/enable"
sudo cat /sys/kernel/debug/tracing/trace
```

The block number passed to the failing `beamfs_free_block` is the
`blk=0x...` value of the entry matching the `Comm=rm` task. Cross-
reference that block against the canonical .beamfs image using
`beamfs_audit2.py <image> <block>` to identify the inode and pathname
that legitimately owns it. The bug is whatever subsequently caused a
**different** inode to also list that block in its pointer tree.

Tooling produced 2026-05-15 (deferred for archival in a future commit):
`beamfs_dump.py` (reverse map block -> inode + path),
`beamfs_audit.py` (full image audit: double allocations, bitmap
coherence, leaked blocks; reports zero issues on the canonical image),
`beamfs_audit2.py` (audit with metadata sharing analysis and
hardlink dump). All three are standalone Python read-only inspectors.

**Session 2026-05-15 late investigation (deterministic reproduction
and partial fix attempt).** The bug WAS reproduced deterministically
in the second half of this session: starting from a fresh canonical
.beamfs deploy on the master VM, the first sysvinit reboot cycle
(`sleep 1 && /sbin/reboot` from inside the VM) produces the
`double free of block N` warn on every run. Eight consecutive
reboot cycles all reproduced the warn (8/8). The block number is
stable within a sub-run of cycles (276098 for the first three cycles
on the test of record, then drifting to 276095 from cycle 4 on as
the bitmap state diverged from canonical).

A runtime kprobe trace captured late in the boot (capture at
t ≈ 80s post-boot, well after the t < 30s where the previous
capture had stopped) revealed eight events on boot 0: one
`beamfs_dir_get_block` allocation, one `S01hostname-cmd` truncate-
down free, four `kworker beamfs_inline_lookup_or_alloc_phys`
allocations clustered at t=14.6s, one `dbus-uuidgen` allocation at
t=17.0s, and two late `kworker` allocations at t=45.3s and t=50.4s.
The last two were the missing data points from the earlier trace:
block 276098 IS legitimately allocated at t=45.3s by a kworker
running `beamfs_inline_lookup_or_alloc_phys`, and post-shutdown
audit of the resulting on-disk image confirms that block 276098
is referenced by inode 28338 (mode 0o100600, size 512 bytes,
direct[0]=276098) and the bitmap bit is correctly cleared
(ALLOCATED).

So the on-disk state at the end of boot 0 is internally consistent:
the canary at the next boot is NOT caused by an inode/bitmap
desync persisted to disk. The desync occurs in-memory during the
reboot itself, between alloc/dirty time and the kernel_restart
syscall driven by /sbin/reboot. Specifically:

  - `beamfs_alloc_block` clears the bitmap bit in memory and calls
    `mark_buffer_dirty` on the bitmap buffer head, but does NOT
    call `sync_dirty_buffer`.
  - `beamfs_inline_lookup_or_alloc_phys` sets the new pointer in
    the in-memory inode and calls `mark_inode_dirty`, but does
    NOT call `write_inode_now` or equivalent.
  - The order in which the bdi writeback flusher commits the
    bitmap buffer vs the inode-table buffer is not deterministic
    relative to the reboot.

**Experimental fix attempt (reverted).** A `sync_fs` super-op was
added (super.c, ~62 lines) that on every `sync(2)`, `fsync`,
`umount` or final pre-reboot sync iterates the bitmap buffer-head
array `sbi->s_bitmap_blkhs[k]` and the superblock buffer head
`sbi->s_sbh`, calling `sync_dirty_buffer` on each (or
`write_dirty_buffer(bh, 0)` when called with wait=0). The
hypothesis was that forcing the bitmap and superblock to disk
during the VFS sync path would establish a deterministic ordering
bitmap -> sb -> inode-table at sync time, since the inode-table
flush is driven by `sync_inodes_sb` before `sync_fs` is invoked.

Empirical result on the 8-cycle reproduction test: 3/8 cycles
passed (cycles 6, 7, 8), 5/8 cycles still produced the warn.
Cycles 1-2 produced the t ≈ 10s warn on blocks 276098 / 276095
(same pattern as baseline), cycles 3-4 surfaced a NEW failure
mode: the warn fires at t ≈ 3s during the mount path itself on
block 17790, indicating that the corruption is read from on-disk
during bitmap init. This means the desync is not purely in-memory
ordering during writeback; some on-disk state must also be wrong.

The `sync_fs` patch was reverted as insufficient. The session
documented this work but did not commit a fix.

**Next session entry points.**

  1. Capture a kprobe trace on the boot that FAILS at t=3s
     (cycle 3 or 4 of the repro test) to identify which on-disk
     bitmap block is read at mount with a faulty bit and which
     inode legitimately points at the same block. This requires
     enabling kprobes earlier in boot than current setup
     (kprobe_event= cmdline already does this, but tp_printk
     output needs to be matched against the on-disk audit).
  2. Investigate whether `beamfs_init_bitmap` in alloc.c may
     itself flip a bit during RS-correction: any sub-block that
     comes back with `rc > 0` is corrected in memory, marked
     dirty, and synced. If RS correction produces a false-positive
     correction (e.g. when the on-disk parity itself was hit by
     a stale write from a previous reboot), the kernel will
     persist a wrong bit pattern.
  3. Evaluate adding `sync_dirty_buffer` immediately after each
     `mark_buffer_dirty` in `beamfs_alloc_block` and
     `beamfs_free_block` (synchronous bitmap update). This is
     ~52 sync calls worst case (1 per bitmap block per state
     change, but typically only the touched block is dirty),
     measured cost ≈ 1-3ms per allocation. Tradeoff: correctness
     vs throughput on metadata-intensive workloads. Probably
     unacceptable on R19 bench but acceptable for rootfs use.
  4. Consider implementing a minimal journal (write-ahead log
     of bitmap + inode-pointer atomic pairs) since the
     reboot-safety story will not be solid without one.

**Decision.** Triaged. The intermittent single-occurrence warn at
boot is bounded by the canary, non-fatal, and does not block the
rootfs validation campaign. Resolution deferred pending the
follow-up investigation outlined above. The `sync_fs` patch
attempt and its 3/8 partial result are archived as
patch06_sync_fs.py for re-use; the full set of forensic Python
scripts (beamfs_dump.py, beamfs_audit.py, beamfs_audit2.py,
beamfs_audit3.py, beamfs_find_all.py, beamfs_inspect.py) and
trace logs from this session are preserved under
/tmp/beamfs-state-2026-05-15* on spartian-1.

**Session 2026-05-18 follow-up (defensive patches, root cause still
open).** A multi-session investigation extended the 2026-05-15 work
with forensic instrumentation (pr_info on every alloc_block,
free_block, evict_inode, write_bitmap_block with watchdog on the
suspect bit) and a debug-build kernel with PROVE_LOCKING + LOCKDEP +
DEBUG_INFO_DWARF5. Findings refined the 2026-05-15 hypothesis:

  1. The H3 drift (bitmap=free, inode-table=referenced) is
     reproducible at 4/5 runs of the same trigger sequence
     (`100 file create + 50 unlink + drop_caches + halt`) on a
     freshly-deployed canonical .beamfs image, with
     `cache=writethrough` qemu (rules out qemu page-cache loss as
     the sole cause).

  2. The drift consistently targets two inode/block pairs per run:
     (a) `/etc/timestamp` (inode 854, mode=0x81a4, nlink=1) whose
     `direct[0]` is rewritten at boot by sysvinit; (b) a transient
     inode allocated during the trigger (inode 28365 in the test
     of record). The specific block numbers vary across runs
     (17790/19657/19148 for inode 854; 276398/279446 for inode
     28365) but always follow the same allocation-order pattern.

  3. `dump_stack()` instrumentation in `beamfs_free_block` did
     NOT fire on the suspect blocks (the bits that ended up
     drifted to `free` on disk). Combined with the grep of
     `set_bit(.*s_block_bitmap)` showing only ONE site in the
     entire codebase (the `free_block` body itself), this is
     **empirical proof that the bitmap bit was not flipped to 1
     by any `free_block` call** in the boot window. Some other
     code path is writing to either `s_block_bitmap` (RAM) or
     `bh->b_data` (the bitmap buffer head) without going through
     the `alloc.c` accessor functions.

  4. LOCKDEP with PROVE_LOCKING enabled produced ZERO warnings
     during boot + trigger. This rules out locking inversion,
     missing-lock-while-held, sleep-in-atomic, or RCU violations
     as the cause. The race -- if there is one -- is a data race
     that lockdep cannot detect by construction.

  5. KCSAN (the kernel data-race detector that would close the
     analytical gap) cannot be enabled on this kernel + arm64
     target: the merge_config silently drops `CONFIG_KCSAN=y`
     during `make oldconfig`. Determining whether arm64 kernel
     7.0.3 supports KCSAN at all (`select HAVE_ARCH_KCSAN` in
     `arch/arm64/Kconfig`) is the next investigation step.

**Defensive patches landed in this session** (semantically correct
even though they do not close §3.10 alone):

  - **C1: `mark_buffer_dirty_inode(bh, owner)`** in
    `alloc.c::beamfs_write_bitmap_block`. Binds the bitmap buffer
    head to the inode whose `direct[]` change triggered the
    allocation, so the VFS `__writeback_single_inode` path runs
    `sync_mapping_buffers` first, establishing bitmap-before-
    inode ordering at writeback time.

  - **D2: `beamfs_sync_fs` super-op** (super.c). Iterates
    `sbi->s_bitmap_blkhs[k]` and `sbi->s_sbh` and calls
    `sync_dirty_buffer` on each. The kernel sync_filesystem
    sequence places this hook BEFORE `sync_blockdev_nowait`, so
    the bitmap reaches disk before the inode-table buffer is
    even submitted. Same ordering hypothesis as C1 but at the
    sync(2)/fsync/umount level rather than per-allocation.

  - **`lock_buffer(bh)`** in `beamfs_write_bitmap_block` around
    the memset + reconstruction + RS-encode + mark_buffer_dirty
    sequence. Closes the torn-write race between the bh
    reconstruction and a concurrent BDI flusher reading
    `bh->b_data` for I/O. Required release of `sbi->s_lock`
    before calling `write_bitmap_block` (lock_buffer can sleep);
    callers refactored accordingly. The bitmap RAM mutation
    stays under `s_lock` (the bh reconstruction reads it via
    atomic `test_bit`, no lock needed).

  - **owner parameter** added to `beamfs_alloc_block` and
    `beamfs_free_block` signatures, propagated through all
    callers (`file.c`, `file_inline.c`, `namei.c`, `super.c`).
    Required by C1; harmless for paths that pass NULL (mount-
    time RS auto-correction in `beamfs_setup_bitmap`).

These patches do NOT close §3.10 in isolation: the 4/5 drift rate
persists. They DO close real races identified by static reading
(torn-write on bh, missing writeback ordering hint). Keep them.

**Next session entry points (revised).**

  1. Determine whether arm64 kernel 7.0.3 has `HAVE_ARCH_KCSAN`.
     If yes, fix the merge_config drop and rerun with KCSAN
     active. If no, evaluate KCSAN backport vs upgrade to
     kernel 7.0.4+ where arm64 KCSAN landed.

  2. Inspect `beamfs_inline_writeback_folio` (file_inline.c
     :1284-1488) for potential off-by-one or wrong-block writes
     into `bh->b_data` of a buffer head that happens to belong
     to a bitmap block rather than a data block. The function
     does `lock_buffer(bh)` correctly, but the `phys` value is
     produced by `beamfs_inline_lookup_or_alloc_phys` and used
     immediately for `sb_bread(sb, phys)` -- if `phys` collides
     with a bitmap block number, the writeback path would
     legitimately overwrite a bitmap block. Verify the
     allocator never returns block numbers in the bitmap
     range (`[bitmap_blk, data_start)`).

  3. Read kernel mainline for any guidance on multi-block FS
     bitmap management with per-block RS FEC. The reconstruct-
     from-RAM approach is unusual; ext4 patches the byte
     in-place under `lock_buffer` rather than full memset +
     reconstruction.

  4. If KCSAN cannot be enabled, fall back to bpftrace kfunc
     hooks (with the kernel's DWARF5 info now exposed) to
     instrument `bh->b_data` reads and writes globally,
     correlating any unexpected write to bitmap-bh regions
     with the call stack of the offending kernel thread.

The Yocto debug build infrastructure (PROVE_LOCKING + LOCKDEP +
DEBUG_INFO_DWARF5 via `beamfs-debug.cfg` fragment, merged through
`linux-mainline_%.bbappend`) is preserved for future sessions.

**CLOSED 2026-07-08 -- root cause identified and fixed.**

The true root cause was NOT the bitmap-before-inode ordering race
hypothesized above (though those mitigations remain valid hardening).
The actual bug: `beamfs_free_block()` in `alloc.c` called
`mark_buffer_dirty_inode(bh, owner)` with `owner` set to the inode
being evicted. This appended bitmap buffer_heads to the dying
inode's `i_data.i_private_list` AFTER `truncate_inode_pages_final()`
had cleared it. When VFS then called `clear_inode()`, the assertion
`BUG_ON(!list_empty(&inode->i_data.i_private_list))` at
`fs/inode.c:801` fired, producing a kernel BUG/Oops (not just the
WARN-level canary documented above -- the earlier sessions observed
the canary because the bitmap writeback ordering masked the
i_private_list pollution in some timing windows).

**Trigger path (deterministic reproducer):** `depmod -a` on
rootfs=beamfs performs `renameat2()` replacing `modules.dep`, which
evicts the old inode via `beamfs_evict_inode` ->
`beamfs_free_data_blocks` -> `beamfs_free_block(sb, blk, inode)` ->
`mark_buffer_dirty_inode(bh, inode)` -- polluting the private list
of the inode currently being torn down.

**Fix (three layers, applied incrementally):**
1. *Definitive fix* (commit 11c844f): call `invalidate_inode_buffers(inode)`
   immediately before `clear_inode(inode)` in `beamfs_evict_inode()`.
   This is the standard VFS pattern used by ext2, minixfs, and all
   buffer_head-based filesystems to detach orphan bh entries from
   `inode->i_data.i_private_list` before the VFS asserts it empty.
2. *Defense-in-depth* (commit f964b27): guard `mark_buffer_dirty_inode()`
   in `beamfs_write_bitmap_block()` with `!(inode_state_read_once(owner)
   & I_FREEING)` to prevent attachment to dying inodes at the source.
   Centralized: covers all 25+ call sites without per-site patching.
3. *Eviction-path hardening* (commit f146cbd): pass `NULL` as owner in
   all 3 `beamfs_free_block()` calls within `beamfs_free_data_blocks()`
   and defer `beamfs_free_inode_num()` to after `clear_inode()`.

**Validation:** 10/10 `depmod -a` on rootfs=beamfs with tainted=0;
full `beamfs-bench full` R19 pipeline exit 0 on 4-node cluster under
RadFI injection (5 FS x 3 probabilities, tainted=4096 OOT-only on
all 4 nodes, zero DIE). Manifest GPG-signed.

**Recurrence observed 2026-08-14 (post-close, informational).**

A single `beamfs: double free of block N` WARN (Comm=rm, canary path,
no crash/Oops, bitmap left consistent) recurred on `beamfs-compute02`
during a routine R19 run at HEAD `28095b78` (descendant of the
three-layer fix `11c844f`/`f964b27`/`f146cbd`, all three confirmed
present in the compiled source). Trigger context: `drop_caches` +
remount + `rm` during a cluster `verify` cycle -- different from the
`depmod -a` reproducer validated 10/10 at closure. `beamfs-bench full`
still exited 0, dmesg reported clean, no fatal regression. Not treated
as a regression of the fix (the code paths it touches are all present
and correct); recorded because the closure's claim that the
WARN-canary and the BUG_ON/Oops share a single root cause was
asserted from the `depmod -a` evidence and has not been independently
re-verified under this trigger. Left as an open question for a future
investigation session, not reopened for action.

*Noted by Claude Sonnet 5 during this session; flagged explicitly per
user request, given a documented general caution about over-reading
signal into benign variance -- treat this entry as a data point to
weigh, not a claim.*

---

### 3.11 RS(255,239) silent miscorrection on data blocks under high-density EM injection (RESOLVED 2026-07-09 via DATA_CSUM, format-v6)

**Symptom.** During the 2026-05-15 publication-grade R19 run on
commit `caf9caf`, the cluster `cluster_attack` phase at
`probability=100000` ppm produced the following observation on
`beamfs-master` (target `dir-B/file-B2.bin`, 262144 bytes):

```
CALL_DELTA=244 FLIP_DELTA=22 RS_CORRECTED=10
DMESG_UNCORRECTABLE=0 DMESG_EIO=0
HASH_PRE  = c7a55e38edc01567c3340d6405fa6618ce98fd18b3275072797feafc3a499add
HASH_POST = 43a9eae0ba38366cc566aa897142a3be0995a1728b12ec2b94bb868a2c82879c
BITS_DIFF=15250 FRAC_CORRUPT=72 HAMM_BLOCKS=2
```

22 single-bit flips were placed by `emufi` across the read path of
the target file. The kernel decode logged 10 subblock corrections
across two physical blocks of the file (`ino=11`, `iblock`s 3, 20,
21 then 11, 12). All ten `decode_rs8()` calls returned `nerr > 0`
indicating success, and zero `uncorrectable` events were logged.
Yet the `cat $TARGET_FILE` output had a different SHA-256 than the
pre-attack content: 15250 bits (~23% of two 4 KiB blocks of the
file) differed between `HASH_PRE` and `HASH_POST`. The verdict
derivation in `beamfs-bench` synthesis.rs correctly classified this
as `verdict=CORRUPTED_DATA` / `verdict_detail=SILENT_CORRUPTION`,
producing exit code 1 on the R19 pipeline.

The other three cluster nodes at the same probability passed cleanly:
- `compute01`: `CALL_DELTA=0` (injector never armed; see 3.12 below)
- `compute02`: `FLIP_DELTA=17 RS_CORRECTED=10 BITS_DIFF=0` -> `RS_RECOVERED`
- `compute03`: `FLIP_DELTA=20 RS_CORRECTED=10 BITS_DIFF=0` -> `RS_RECOVERED`

The same R19 sequence was run earlier in the day on commit `a97c09b`
(no code differences in the data path, only a documentation update
between the two commits) and passed with `BITS_DIFF=0` on master at
the same probability. The bug is therefore non-deterministic and
statistical.

**Root cause.** `beamfs_inline_decode_block_into_buf` (file_inline.c)
loops over the 16 RS(255,239) subblocks of a data block, calling
`beamfs_rs_decode_region` on each. The kernel library decoder
returns the number of corrected symbols (`nerr`). The current code
accepts any `nerr >= 0` as a successful correction. For RS(255,239),
the maximum number of correctable symbol errors per codeword is
`(255 - 239) / 2 = 8`. Above this bound, `decode_rs8()` will either
return `-EBADMSG` (uncorrectable detected) or, with non-zero
probability, return `nerr >= 0` with a **different valid codeword**
than the one originally encoded. The latter outcome is a "silent
miscorrection": from the decoder's view the result is internally
consistent (parity matches the corrected data); from the
application's view the bytes returned are not the bytes that were
written.

The `emufi` injector parameters in effect during this R19 were
`flip_locality=EMUFI_LOC_ADJACENT, flip_width=1, flip_stride_bits=8`,
which places single-bit flips at adjacent positions on each
`submit_bio` invocation. Across the 244 calls that fired against the
target file's read path, 22 flips landed; statistical clustering
caused at least one subblock (most likely two) to receive more than
8 flips, exceeding the RS error budget. The decoder then converged
to a different valid codeword, the kernel logged 10 "subblock
corrected" lines (mismatching the actual flip pattern), and the
read returned the wrong bytes.

The inode read path in `inode.c::beamfs_iget` is **not** affected by
this class of bug because, after `decode_rs8` returns success, it
recomputes the CRC32 of the corrected raw inode and rejects the
decode if CRC32 still mismatches. The data-block path lacks this
defense: the on-disk format v5 does not reserve space for a per-
block CRC32. The 16-byte tail padding of the block (bytes 4080-4096)
is zero-initialized at write time and is not affected by RS decode,
but it is too small (128 bits) to serve as a reliable integrity
check against a Reed-Solomon miscorrection that operates on the
4080-byte codeword payload.

**Impact.** Under EM injection at probability >= 100000 ppm
(saturating regime), beamfs can return incorrect data on `read(2)`
without any kernel-visible signal. The corruption is silent: no
`pr_warn`, no `-EIO`, no `dmesg` "uncorrectable" line. Cluster
verify on disjoint files of the same partition shows no on-disk
divergence because the on-disk bytes are correctly persisted; the
corruption is entirely in the read decode path.

This failure mode is intrinsic to any RS-only FEC layout: it is
demonstrated and well-documented in the coding-theory literature
under "decoder miscorrection probability". For RS(n,k) with
correctable bound `t = (n-k)/2`, the conditional probability of
miscorrection given a received word at distance `> t` from any
codeword is bounded by `t! / (q^t * t!)`-style expressions that are
small but non-zero. In practice, with bursts placed by `emufi` on a
single subblock, the probability is high enough to be empirically
observable within the bench duration.

**Risk.** A user-space process reading data through beamfs under
heavy EM stress can receive plausibly-shaped but incorrect bytes.
Higher-level integrity checks (application CRC, filesystem-of-
filesystems checksum, or distributed agreement across multiple
cluster nodes) are the only current defenses. For the cluster
workload, the per-node hash check at `cluster_verify` time **does**
expose the miscorrection: `extract_cluster_verdict_detail` flags
`SILENT_CORRUPTION` on hash mismatch with no kernel signal, which is
the empirical capture path used by R19.

For rootfs deployments (no application-level CRC), this means
beamfs cannot be relied upon to deliver byte-perfect rootfs files
under sustained EM injection above the RS saturation threshold.
Below the saturation threshold (probability <= 10000 ppm with
`emufi` default locality), no such event has been observed across
multiple R19 runs.

**What is known about the cause.**

1. The decode path in `file_inline.c::beamfs_inline_decode_block_into_buf`
   loops over the 16 subblocks of a data block via
   `beamfs_rs_decode_region` and accepts any `nerr >= 0` return.
2. There is no post-decode integrity check on the decoded buffer.
   Unlike `inode.c::beamfs_iget`, which recomputes CRC32 after a
   successful RS decode and rejects on mismatch, the data-block
   path has no second source of truth.
3. The on-disk format v5 has no per-block CRC32 field. The 16-byte
   tail pad of each disk block is zeroed at encode time, but is
   below the RS codeword payload and would not detect the failure
   modes observed.
4. The bug is statistical: same code, same probability setting, same
   injector configuration produced `BITS_DIFF=0` on R19 run 1 and
   `BITS_DIFF=15250` on R19 run 2 within the same day. Run-to-run
   variance is driven by the PRNG seed of the injector and the
   timing of `submit_bio` calls relative to the page-cache miss
   sequence.

**Mitigations considered.**

  - **(M1) Strict nerr bound.** Reject any `decode_rs8` return with
    `nerr >= threshold_low` (e.g. 5 out of 8 max) as
    uncorrectable. Pro: zero on-disk format change. Con: rejects a
    fraction of legitimate corrections, reducing the RS error
    budget below its theoretical capacity. Empirical impact on R19
    pass rate not yet measured.
  - **(M2) Per-block CRC32 in on-disk format v6.** Reserve 4 bytes
    per data block (e.g. relocate to bytes 4076-4080, shrinking
    user payload from 3824 to 3820 bytes per block) for a CRC32 of
    the codeword payload. After `decode_rs8` returns success,
    recompute CRC32 and reject on mismatch. Pro: full RS capacity
    preserved, defense identical in spirit to the inode path. Con:
    on-disk format bump (v5 -> v6), `mkfs.beamfs` update, kernel
    reader update, migration path for existing v5 images, RS
    parity recomputation, and Yocto recipe update. Multi-session
    work.
  - **(M3) Cluster-level voting.** When cluster_verify detects
    hash mismatch on one node and not others, treat as locally-
    silent miscorruption and recover from a quorum node. This is
    the empirical defense path used by the cluster bench but is
    not available to single-node rootfs deployments.

**Decision.** Triaged. The bug is statistical, pre-existing across
multiple commits prior to this session, and intrinsic to RS-only
FEC without a per-block CRC. Resolution requires (M2), which is an
on-disk format bump scheduled as a separate roadmap item under
`format-v6 + per-block CRC32`. Until then, R19 runs that hit the
saturation regime will occasionally fail with `verdict=CORRUPTED_DATA`
/ `verdict_detail=SILENT_CORRUPTION` on one or more nodes; this is
expected behavior given the current FEC capacity, not a regression.
The empirical record is preserved in
`Documentation/runs/beamfs-bench-analyse-full-20260515-210627` on
spartian-1 (forensic tarball
`/tmp/beamfs-bench-analyse-full-20260515-210627.tar.gz`).

**Resolution (2026-07-09).** Mitigation (M2) was implemented as the
DATA_CSUM feature (on-disk format-v6): a per-data-block CRC32 stored in
the 16-byte block tail pad, gated by RO_COMPAT bit 4, recomputed after
RS decode and compared with the stored value. On mismatch the read
fails closed (-EIO) and emits an UNCORRECTABLE journal entry. The CRC32
is a soundness predicate independent of the Reed-Solomon algebra, so a
codeword-valid but originally-unencoded payload is rejected with
probability at least 1 - 2^-32 per event. This discharges the
antecedent of Theorem v2.2b for adversary Family A.

The fix was validated empirically with a deterministic reproduction of
this failure mode, rather than by waiting for a rare stochastic
miscorrection. emufi 0.4.0 adds the EMUFI_LOC_CODEWORD_SUBST mode
(flip_locality=5): it overwrites one RS codeword window with the
all-zero codeword, valid by linearity. decode_rs8 then succeeds
(nerr=0) and returns a zero payload differing from the original: the
deterministic analogue of the silent miscorrection described above.

Result (run `beamfs-bench-analyse-full-20260709-221712`, injector
emufi, flip_locality=5, prob=1000000, DATA_CSUM active):

- `dmesg`: `beamfs/inline: ... data_csum mismatch want=... got=...`
- `cluster-records.txt`: `CAT_RC=1`, `DMESG_UNCORRECTABLE>0` on 4 nodes
- `multifs-synthesis.md`: verdict `RS_FAIL_CLOSED` / `DETECTED_FAIL_CLOSED`
- manifest `manifest-20260709T202958Z.json.asc` (GPG-signed, overall_rc=0)

Without DATA_CSUM the same substitution reads back silently wrong
(`SILENT_CORRUPTION`); with it, `RS_FAIL_CLOSED`. The run and manifest
are archived under `Documentation/runs/` (promoted via `git add -f`).
DATA_CSUM is opt-in (`mkfs.beamfs --data-csum`); the default path is
byte-identical to v5. Family B (adversarial saturation) can defeat the
unkeyed CRC32 and is not claimed here; keyed integrity is reserved via
the `csum_type` field.

---

### 3.12 emufi injector remains disabled on compute01 across all probabilities (TRIAGED 2026-05-15)

**Symptom.** Across all three probabilities of the cluster phase
(1000, 100000, 1000000 ppm) of the 2026-05-15 R19 run,
`beamfs-compute01` consistently reports `CALL_DELTA=0`,
`FLIP_DELTA=0`, indicating that the `emufi` injector was never
exercised on this node despite the bench harness arming it. The
other three nodes (master, compute02, compute03) show normal
`CALL_DELTA` values of 240-250 per probability iteration.

**Impact.** The cluster experiment loses one of its four nodes for
the fault-injection campaign; effectively a 3-node test rather than
4-node. R19 verdict derivation correctly classifies compute01 as
`RS_PASSTHROUGH` (no flips placed -> hash matches) at every
probability, which masks the issue at the pass/fail level but
reduces statistical confidence in cluster-level claims.

**What is known.** The injector counter `target_dev` is correctly
written by `worker.sh::cluster_attack` (verified in the bench source);
`hook_blk` is set to 1 in the same code path. The `lsmod` output in
the compute01 forensic capture confirms `emufi` is loaded. The
absence of `CALL_DELTA` increments suggests the hook is registered
but `bh->b_bdev->bd_dev` on the target submit_bio path does not match
the packed `target_dev` value on this node. Possible causes: vdb
major/minor numbering differs on compute01 vs the other nodes, or
the udev/virtio-blk probe order produces a different device-number
pairing.

**Decision.** Triaged. Side-investigation deferred. The cluster
3-node-effective behaviour is documented in the bench output and
does not block R19 from passing or failing on the other nodes; the
empirical record is sufficient for the present session.

---

### 3.13 Indirect parity describes the block's previous owner (PARTIALLY ADDRESSED 2026-09-18)

**Symptom.** `generic/083` fails on every trial when the volume is
made with the default `--indirect-parity=rs`, losing 238 to 636
blocks per run; with `crc` it fails four times in ten losing 11 to
282; with `none` it fails three times in ten losing 7 to 86. Same
kernel, same image, ten trials each, measured outside the harness.

**Mechanism.** The parity region keeps one signature per block and
nothing cleared it when a block was freed. The allocator hands the
block to another file, and `beamfs_ind_parity_verify` then measures
the new contents against the old owner's signature. `capsule.md`
states the same thing: "the next owner inherits a description of the
previous one's contents and verify reports the new block as corrupt
against it."

What the two modes do with that verdict is what separates them.
Under `crc` the block is refused: `beamfs_del_dirent` walks the
directory through `beamfs_dir_get_block`, which verifies every
indirect block it reads, and one `-EUCLEAN` there returns through
`beamfs_rename` *after* it has already added the new entry --
"del_dirent failed after add, fs may be inconsistent". In ten trials
the four that logged that message are exactly the four that failed,
each leaving an inode no directory reaches; the six that did not log
it passed with nothing lost.

Under `rs` the block is *corrected in place*, which is worse: the
indirect block is rewritten toward what it held under its previous
owner, erasing every pointer installed since. That is the shape of
the 238-to-636 block losses, and it is why the mode that protects
least loses least.

**Attempted and reverted** (2026-09-18):
`beamfs_ind_parity_forget()` zeroed a block's slot in
`beamfs_free_block()`. Measured over ten trials each under `-I crc`:
without it 6 pass / 4 fail and 214 parity false positives, with it
0 pass / 10 fail and 322 -- sixty points against a spread of
forty-four, and the false positives it targeted up by half.

It did a decode-modify-re-encode of the region block followed by a
plain `mark_buffer_dirty` with no inode to carry it, which is the
failure `ind_parity_update` documents at length: the region changes
in memory, the decode cache is invalidated, and the write may never
land. Memory and medium then disagree, and a later write of the
region from a stale copy takes the legitimate updates with it.

Clearing at free time is not the answer. Telling a stale signature
from a valid one is, and that is the per-block generation below.

**Still open.** Freeing is not the only way a signature goes stale.
`beamfs_ind_parity_update` itself documents two others: a block
written with no scratch page available "goes to the medium
undescribed", and a parity region that is never flushed leaves the
medium holding the parity of what the block contained before. Until
a block carries something that lets a reader tell a stale signature
from a valid one, `rs` converts any such desynchronisation into
corruption. `capsule.md` designs exactly that -- a per-block
generation, outside the codewords -- and it exists for data capsules
only. The indirect blocks, whose parity is the one that destroys,
do not have it.

**Consequence for deployment.** `--indirect-parity=rs` is the mkfs
default and must not be shipped as such until the generation covers
indirect blocks. In its present state it is more dangerous than
`none`: `none` leaves a desynchronisation inert, `rs` acts on it.

---

### 3.14 The read path reached the buffer cache from the fault path (RESOLVED 2026-09-18)

**Symptom.** Under sustained load the node stops: 270% CPU, `kswapd`
running, eight tasks on the same line of
`beamfs_inline_read_folio_range`, nothing advancing, SSH refused at
the banner. Captured 2026-09-18 during `generic/083` under
`-I crc`, kept as evidence. The same shape had been recorded before
as a `generic/464` wedge.

**Mechanism.** `read_folio_range` read each block with `sb_bread`.
`__bread_gfp` adds `__GFP_NOFAIL` whatever mask it is given, so the
read cannot fail: on a machine whose page cache is already full of
this filesystem's own blocks, it loops in the allocator instead.
The block-device cache also holds a second copy of every block the
page cache already has, which is what fills memory in the first
place -- the guest's resident size grew from 3.0 to 8.4 GiB across
three baseline series.

**Resolution.** The path no longer creates a buffer. It consults the
cache with `sb_find_get_block`, which never allocates, and uses that
copy when a dirty buffer holds newer bytes than the medium;
otherwise it reads the block straight into a scratch page with
`bdev_rw_virt`. The scratch pool is bounded and may fail, which is
the point: `-ENOMEM` returned cleanly instead of an allocation that
cannot fail. The capsule being exactly `BEAMFS_BLOCK_SIZE` is what
makes the single-page read possible with no bounce.

`beamfs_inline_decode_block_into_buf` now takes a raw buffer rather
than a `buffer_head`; the four callers that hold the buffer locked
pass `bh->b_data` and keep their locking contract unchanged.

**Verified.** Ten consecutive trials under `-I crc` with no wedge,
where the node had previously stopped on the fifth. Ten trials do
not prove a wedge absent; what is established is that the cause
named in the code was removed and the symptom did not recur.

The symptom did recur, on 2026-09-21, after thirty trials; 3.15 is
what it measures as.

### 3.15 File folios held after their mapping drops them (FIXED 2026-09-22; 1 % residual open)

**Symptom.** The same stop as 3.14: page allocation stalls of 10 to
43 seconds on `xfs_io`, `sh`, `klogd` and on `beamfs-scrub` inside
`__bread_gfp`, an RCU stall on `kmemleak`, no OOM kill because the
allocator reports `all_unreclaimable? no`, and ssh refused at the
banner. Serial log of `trace generic/464`, 2026-09-21 evening.

**Measurement.** At the stall, `Mem-Info` gave `active_file +
inactive_file` = 1 733 294 pages, 6.6 GiB of 8, and `total pagecache
pages` = 29 107, 114 MiB. The second is `NR_FILE_PAGES`, folios that
some `address_space` holds; the first is the file LRU. A folio leaves
`NR_FILE_PAGES` when `__filemap_remove_folio` takes it out of its
mapping and leaves the LRU only when it is freed. The difference,
6.5 GiB, is folios that their file or block device dropped and that
something still holds a reference to.

The same difference is in `/proc/meminfo` as `Active(file) +
Inactive(file) - (Cached - Shmem) - Buffers`. Over the 23-test sweep
1789974744 of the same day, read test by test from the captures:
zero through generic/012, then +146 MiB on generic/013, unchanged
across 014 and the budget-killed 074, +496 MiB on 075, +28 MiB on
076, +212 MiB on 083, then flat at 887 MiB through 102, 109, 269,
464 and 476. It never fell: not on unmount, not on mkfs, not across
twenty-three tests. After the 734-test sweep of the night of the
21st and three trials of 476 it stood at 1 822 168 kB, 1.74 GiB, and
a trial of generic/001 added nothing.

`beamfs-xfstests` 2.3.7 derives it as `mem.orphan_file` at every
capture and prints it at the start and end of a run; before 2.3.7
the guest probe had never returned a value at all (quoting fault,
204 trials of 083 with host counters only).

**Established.** The pool exists, is not reclaimable, grows in steps
on four of twenty-three tests and on none of the write-heavy ones,
and is what fills the guest until the allocator stalls. 3.14
removed a real cause; its "resident size grew from 3.0 to 8.4 GiB"
was this pool seen from the host, and it is still there.

**Cause, measured 2026-09-22.** Not an extra reference at the exit
of the cache. From a freshly booted node, `beamfs-xfstests` 2.3.12
(`heldfolio` on the `mm_filemap_delete_from_page_cache` tracepoint,
which both removal paths fire) saw 282 939 and 294 669 folios leave
the cache in two trials of generic/083, all but a dozen at exactly
two references, and some seventy held ones in known transient places
(`aio_free_ring`, shmem eviction, a concurrent reader under
truncate) -- while the pool grew 196 MiB a trial. What that version
declared normal was the finding: 2.3.14 counted folios leaving with
`private` still set, and found 50 752 and 52 235 per trial, all at
`blkdev_flush_mapping` when the scratch device is released, each
carrying a `buffer_head` with `b_count` from 1 to 7. 411 948 kB of
them against 411 940 kB of pool growth. A buffer someone still holds
survives `block_invalidate_folio`, its folio leaves the mapping at
three references, batch and cache let go, the buffer's does not, and
a folio out of every mapping but never freed stays on the LRU.

`bhbalance` (2.3.15) then balanced every `sb_bread`, `sb_getblk` and
`sb_find_get_block` against every `brelse` and `bforget` by calling
function: beamfs returns what it takes through those, to about 500
references a trial. The 50 000 were taken elsewhere: `get_bh` before
`bh_submit` in `beamfs_inline_writeback_range`. In 7.3 `bh_submit`
takes no reference and `bh_end_write` drops none (fs/buffer.c 1085
and 200: the caller holds the buffer across the I/O), unlike the
`submit_bh` and `end_buffer_write_sync` pair the code was written
against, where the first took one and the second gave it back. The
last block of a range had beamfs's own completion with its `put_bh`;
every other block was submitted with `bh_end_write` and its
reference had no taker. One per block written, as many as the block
was rewritten: `b_count` 1 on 40 000 blocks, 2 or 3 on 11 000, more
on 400.

**Fix.** `beamfs_inline_wb_end_block`: what `bh_end_write` does,
then the `put_bh` that matches the `get_bh`. One site; `get_bh`
appears nowhere else in beamfs.

**Verified.** Same node, same probe, freshly deployed: 540 and 521
folios left with a buffer attached (`b_count` 1 on all but two), and
the pool grew 4 236 kB across the two trials against 411 940 kB
before, a factor of 97. generic/083 itself is unchanged, one pass and
one fail, with leak-1 caught live on the failing trial as before.

**Residual, open.** About 530 buffers a trial, 2 MiB, still leave
the device with `b_count` 1. `bhbalance` on the unpatched kernel
showed a deficit of 489 on `beamfs_inline_lookup_or_alloc_phys_new`,
an `sb_getblk` handed to `beamfs_ind_parity_update` in the
iomap_begin path with no release paired to it; same order, same
kind of buffer. Hypothesis, not yet read in the source. It is 1 %
of what wedge-1 was and does not fill a guest in a night.

### 3.16 Writeback stops under generic/074 (OPEN, measured 2026-09-22)

**Symptom.** `fstest` with `mmap=1`, a 10 MiB file, ten loops: the
first two runs of the test finish, cleanup included; the third never
prints its header. The node then sits for the whole budget with
`Dirty` 194 824 kB, `Writeback` 0 and no I/O in flight on the test
device (`/proc/diskstats`, `/proc/vmstat`: `nr_dirty` 48 706,
`nr_writeback` 0). The kernel says nothing for thirty-one minutes;
the hung-task detector did not fire. Reproduced twice out of the last
thirteen runs of the test (sweep 1790087123, sweep 1790090362); the
test passes the other times.

**Established.** The flusher submits nothing with 190 MiB of dirty
pages ahead of it. That is either a buffer it finds locked on every
pass and redirties without writing, or a lock cycle between the
writeback path and something holding a folio or buffer of the block
device's cache. Which one is not known: no stack of a stuck task was
taken. `beamfs-xfstests` 2.3.18 takes them (`stall.txt`) at the next
occurrence.

**Consequences on the harness, closed in beamfs-xfstests 2.3.18.**
The sweep went on to the next test with the stuck tasks still
holding the device and ran mkfs over it; generic/075 was then
recorded twice with 12 000-odd lost blocks, and once with 59 351
blocks referenced by an inode and free in the bitmap. 075 alone
passes ten benches and two sweeps: those failures were made by the
harness, not by the filesystem. The 074 fsck in the evidence was
taken on the mounted device and is not evidence either.

**Not to confuse with.** 3.14 and 3.15 stalled the guest for lack
of memory; here memory is free (6 GiB) and the disk is idle.

**Where to read while waiting for the stacks.** `file_inline.c`
around `beamfs_inline_writeback_range` describes a flusher sitting in
`__lock_buffer` for an hour and a four-party cycle through the block
device's page cache (generic/076); 074 reaches a similar state
through mmap writes and the unlink of a 10 MiB file.


### 3.17 Direct-pointer allocation check inoperative (FIXED 2026-09-22)

**Symptom.** None visible, which is the defect. `beamfs_inline_lookup_phys`
bounds-checks a direct pointer against `[s_data_start, s_data_start +
s_nblocks)` and then asks `beamfs_block_is_allocated` about it -- but
asked about `phys`, a local not yet assigned in that branch, instead of
`dphys`. The message "unallocated direct pointer" appears in no evidence
file of any run on record (16 096 files searched), so either the stale
stack value always fell below `s_data_start` (reserved zone, answered
"allocated") or the compiler dropped the branch as undefined. Either way
a direct pointer into a freed block passed unseen, while the same case
on an indirect pointer is caught by `beamfs_check_intermediate_block`.

**Found by.** cppcheck 2.13 `uninitvar` on the d14ad29 tree, confirmed
by reading; the only behavioural defect out of 32 tool findings, the
rest being three-line NULL checks the scanner misread.

**Fix.** `phys` -> `dphys` at the call and in the message (module
0.1.6). Expect the check to fire on volumes fsck already reports as
holding referenced-but-free blocks (generic/075: 59 351): that is the
check working, not a regression.

### 3.18 INODE_UNIVERSAL scheme kept no parity for indirect blocks (CLOSED 2026-09-22, by refusal)

**Symptom.** `beamfs_iomap_begin` in file.c (scheme 5, "legacy iomap
path", still selected by inode.c and namei.c when `s_scheme` says so)
installs indirect, L1 and L2 pointers at six sites without
`beamfs_ind_parity_update`, and reads them without
`beamfs_ind_parity_verify` -- zero occurrences of `ind_parity` in the
file. mkfs lays out the parity region for every scheme, so such a volume
is born with indirect blocks no parity describes, and fsck reports each
one as never described.

**Decision.** Not implemented, refused: mkfs.beamfs no longer accepts
`--scheme inode-universal`, and mount returns -EINVAL for scheme 5 with
the reason. file.c stays compiled and unreachable; retiring it is a
separate change. No xfstests run ever used this scheme (all UNIVERSAL_INLINE).


### 3.19 Writeback one block at a time, every boundary block twice (FIXED 0.1.7, 2026-09-23)

**Symptom.** generic/074 killed at the 1870 s budget, three runs out of
three; 102 and 476 the same. Measured with iowho and churn (BX 2.3.25,
2.3.26): 2 176 467 writes of exactly one block in 1870 s, 1.12 ms each,
one in flight at a time (2 106 572 ms of write-wait over 1870 s), 4.8
MB/s, 100 % from the flusher, 99.8 % in the data zone; 756 596
allocations all from pwrite, 90 truncates -- the test was in its fifth
pass (3 children, 5 files, 10 loops), progressing at 3 loops a minute
where the budget allowed 31 minutes of a 52-minute run. No task blocked,
no pointer lost. ext2 on the same node, same devices: 074 in 17 s, the
six tests in 3 minutes.

**Cause.** A block carries 3824 bytes of payload, a folio 4096, so
every folio ends inside a block the next folio continues. Writeback
encoded through the block device's buffer cache, one buffer_head and
one bio per block, and took the buffer lock: the boundary block was
read back, decoded, merged, re-encoded and written by the first folio,
then again by the second, which first waited on the lock the write in
flight still held. Every folio waited for the previous folio's bio.

**Fix.** file_inline.c writeback rewritten after the shape of ext2/ext4
(mpage, ext4_io_submit): encoded blocks go into pool pages, contiguous
pages into one bio of up to 32, submitted when the run breaks, when
full, or at the end of the pass; the boundary block waits in the pass
context for the folio that continues it and is encoded and written
once; a folio is finished from the completion of the last block that
carries its bytes, exactly once. No buffer lock is held across anything
that sleeps (the rule generic/076 enforced). Aliases of data blocks in
the buffer cache are brought to the written bytes and made clean before
submission. Also: a failed mount no longer leaks the scratch pool and
the RS staging (out_free_sbi).

**To verify.** The same sweep of generic/074 under iowho: requests
larger than one block, several in flight, the boundary blocks written
once, and the test inside its budget.

### 3.20 One allocation cursor for every writer (FIXED 0.1.8, 2026-09-23)

**Symptom.** After 3.19 (module 0.1.7), generic/074 under iowho: bytes
written halved (8 915 -> 4 482 MiB), queue depth 1 -> 17, and still
killed at the budget. 1 054 631 bios of 1 069 059 carried a single
block: no two consecutive blocks of a file were adjacent on the medium,
so the writeback could not gather them, and the device served 4 KiB
writes at 570 a second, 44 % busy, 32 ms each in the queue.

**Cause.** beamfs_alloc_block used one cursor for the volume
(s_alloc_goal), advanced by whichever writer allocated last and pulled
back to every freed block. Three children writing five files each laid
their blocks down interleaved, and each truncate sent the cursor back
into the holes.

**Fix.** ext2's reservation windows (fs/ext2/balloc.c): a writer's goal
is the block after the last one it received; each inode being written
holds a window of blocks, in memory under s_lock, where no other inode
allocates; 16 blocks to start, doubled each time a window is used up in
order, 512 at most; discarded at evict; ignored when nothing free is
left outside the windows. Allocations without an owner keep the volume
cursor, outside the windows.

**To verify.** sweep generic/074 under iowho: the bio size histogram
should move from [4K, 8K) to [64K, 256K), and the test end inside its
budget.

### 3.21 The tree checker and the parity verify ate the CPU (FIXED 0.1.9, 2026-09-23)

**Symptom.** After 3.19 and 3.20 (module 0.1.8): bios of 128-256 KiB,
the device 43 % busy, and generic/074 no faster. cpuwho (BX 2.3.27):
fstest held 74 % of all CPU time, 13 % idle, 16 s off-CPU in 31
minutes. Of the kernel samples, 68.6 % were under
beamfs_inline_free_blocks_from (the O_TRUNC of each loop) ->
beamfs_free_block -> beamfs_tc_forget_child and beamfs_tc_forget_parent,
46.3 % of them spinning on s_tc_lock; 19 % were decode_rs8 under
beamfs_ind_parity_verify from beamfs_inline_lookup_or_alloc_phys_new.

**Cause.** (1) treecheck.c forget_child and forget_parent walked the
whole hash table, under the global spinlock, for every freed block: a
truncate of 7 500 blocks was 15 000 walks of hundreds of thousands of
entries, and three writers queued on the lock. (2) The decode cache of
the parity region was keyed on one counter bumped by every parity
update; with three writers filing pointers it never hit, and every
pointer lookup decoded the sixteen codewords of its region block.

**Fix.** (1) A second index keyed by child: forget_child is one probe;
forget_parent and zeroed probe the 512 (parent, slot) keys of the block
instead of the table. The checker stays in the image: it is what caught
the lost pointers of generic/650. (2) ext4's buffer_verified: an
indirect block's copy in memory is checked once, when first read, or
marked when we write it with its parity filed; beamfs_ind_parity_verify
returns at once while the bit is set, and the scrub uses
beamfs_ind_parity_verify_medium, which clears it first. "has no parity
written yet" is said once per copy, not once per lookup.

### 3.22 mkfs.beamfs formatted a device the kernel still held (FIXED mkfs 0.1.2, fsck 0.1.3, 2026-09-23)

**Symptom.** generic/650 after a generic/476 killed at the budget: fsck
of the scratch volume after a clean unmount found 24 130 used-but-
unreferenced blocks, directories naming free inodes, 40 indirect blocks
with pointers and no parity. 14 ms into the test, on a volume with
243 701 free blocks, the kernel said "volume full" from two CPUs.

**Cause.** 476's fsstress processes were still alive in sync; the
harness detached the scratch mount lazily and formatted the device;
mkfs.beamfs opened it O_RDWR with no exclusion and formatted it under a
superblock still alive; 650's mount got that superblock back (sget
matches on the device), and when the old writers died the old
superblock flushed its bitmap, inode table and directories over the
new filesystem. mke2fs opens with O_EXCL and refuses a held device
(EBUSY) unless forced twice; in the same harness ext2 fails at mkfs,
xfstests reports "failed to mkfs", and nothing is corrupted.

**Fix.** mkfs.beamfs opens O_EXCL, checks /proc/mounts, refuses a held
device unless -F -F (one -F still means nothing, for scripts that pass
it). fsck.beamfs repairs through O_EXCL and refuses a held device;
check-only reads it as it is. The harness keeps its share: beamfs-
xfstests must not detach a mount lazily and must restart the domain
after a kill (2.3.28).

### 3.23 mkfs and fsck against mke2fs and e2fsck (FIXED mkfs 0.1.3, fsck 0.1.4, 2026-09-23)

Reviewed side by side. Fixed: mkfs left s_uuid and s_label zero (every
volume had the null UUID; now a random v4 UUID, -U and -L as mke2fs);
mkfs never fsync'ed (a power cut after it returned could leave the
superblock, written last, as the one block that never landed); mkfs
had no -V, -q; one usage text still offered the refused scheme; a
duplicated condition. fsck rejected the options fsck(8) and
systemd-fsck pass (-a, -C, -T, -t, -r) with a usage error, so a
boot-time check failed before reading a block; fsck did not compare
the superblock's block count with the device, so a truncated image
passed pass 1 and every later pass read past the end; three %u fed
ints; owner_of leaked on one OOM path.

Still open, and the largest difference: the superblock carries no
state. ext2 records VALID/ERROR, a mount count and a last-check time;
the kernel sets ERROR at the first inconsistency and fsck at boot
knows whether a volume was cleanly unmounted. beamfs has neither a
"mounted" nor an "errors" flag, so nothing tells fsck that a check is
due, and nothing tells the next mount that the last one saw damage.
Kernel change (a bit in s_flags set at rw mount and cleared at clean
unmount, another set by beamfs_fail and every EUCLEAN), with mkfs and
fsck reading it; scheduled with the next kernel build.

### 3.24 fsck --repair wrote repairs with the old parity (FIXED fsck 0.1.5, 2026-09-23)

**Symptom.** generic/650 on module 0.1.5: "bmap blk 0 sub 0 uncor" at
mount, four lost pointers, blocks claimed twice, right after xfstests
had run fsck.beamfs -y on the test device (_repair_test_fs).

**Cause.** Pass 4 regenerated bitmap bits in the payload and wrote the
block with its old parity; pass 1 fixed s_crc32 the same way. Only
pass 5 re-encoded, and said why in its comment (2026-08-25). On the
next mount the kernel's decoder either corrected the regenerated bits
back to the damage (eight symbols or fewer per codeword) or declared
the codeword uncorrectable and alloc.c took the raw bits: a repair
that manufactures the very corruption it was asked to remove. ext4's
metadata_csum has no such state: the checksum is recomputed on every
write of a modified block, without exception.

**Fix.** sb_reencode() for the superblock, rs_encode_subblock for each
bitmap codeword a repair changes. Hypothesis to confirm, not a proven
chain: that the first 650's bitmap damage came from this repair.

### 3.25 The superblock had no state (FIXED module 0.1.10, fsck 0.1.5, 2026-09-23)

ext2's s_state, in the upper half of s_flags: MOUNTED set by a
read-write mount and cleared by a clean unmount, ERRORS set when the
RS journal records an uncorrectable event. A mount that finds either
warns and remembers it (s_unclean); fsck reports both and --repair
clears them after a clean run. Older kernels mask the low sixteen bits
and ignore these. What s_unclean should change in the kernel's
behaviour -- treating a parity mismatch on an indirect block as
unverifiable rather than as damage after an unclean shutdown -- is the
next design decision, and the prerequisite for computing indirect
parity once per flush.

---

## 4. Filesystem feature limitations

These are deliberate scope restrictions of the current
implementation, documented for clarity. They are not defects in the
sense that the implementation matches its current specification;
they are gaps relative to a fully POSIX-compliant general-purpose
filesystem, which beamfs does not currently aim to be.

| Feature | Status |
|---------|--------|
| Maximum file size | Approximately 478 GiB (12 direct + single/double/triple indirect, `BEAMFS_MAX_IBLOCK_TINDIRECT * 3824` bytes). Measured 2026-08-23 on a freshly formatted volume: a 64 MiB file, well into double indirect, was written and read back byte-identical at 24 MB/s with no `EOPNOTSUPP`. Truncate to a smaller size (`file_inline.c::beamfs_inline_free_blocks_from`) frees only direct and single indirect blocks; shrinking a file that reached past 524 blocks leaves the deeper levels allocated (a space leak, not a correctness problem for reads) until sub-step 6 closes it. Deleting the inode (`super.c::beamfs_free_data_blocks`) walks all three levels and leaks nothing. |
| Symbolic links | Fast symlink only. Target stored inline in `fi->i_direct[]` (96 bytes), capped at 95 bytes plus zero terminator. Targets longer than 95 bytes return `-ENAMETOOLONG` at `symlink(2)` time. Slow symlink (data-block target) not implemented. Empirical audit on the `beamfs-rootfs-test.bb` rootfs scratch tree (busybox + dropbear + bash minimal): 388 symlinks total, max target length 34 bytes, distribution concentrated at 16-23 bytes. Fast-symlink suffices for minimal rootfs deployment. Empirical audit on the beamfs-research-image rootfs scratch tree (984 MiB, full HPC stack with slurm, openssl, ca-certificates, iperf3, fio, etc.): 1221 symlinks, max target length 106 bytes, 6 symlinks exceed 95 bytes (all in /etc/ssl/certs/ pointing to long-named Mozilla root CA files in /usr/share/ca-certificates/mozilla/). Verified empirically that mkfs.beamfs --from-dir silently skips these 6 entries with a warning on stderr; they do not appear in the produced .beamfs image. The cluster boots and operates normally (R19 cycle 2026-05-15 exit 0) since these specific CA roots are not used by init, sshd, slurm, munge, or the bench workload. A TLS-heavy workload may notice missing roots and need slow symlink support; current rootfs use is unaffected. |
| Extended attributes (xattr) | Not supported. |
| SELinux labels (`security.selinux` xattr) | Not supported. |
| POSIX ACLs (`system.posix_acl_*` xattr) | Not supported. |
| `RENAME_EXCHANGE` flag | Returns `-EINVAL`. |
| `RENAME_WHITEOUT` flag | Returns `-EINVAL`. |
| `SB_RDONLY` enforcement | Not enforced. Writes to the superblock buffer occur on RS journal updates and bitmap writeback even if the filesystem was mounted read-only. |
| Online filesystem resize (grow/shrink) | Not supported. |
| Quotas | Not supported. |
| Reflinks (`FICLONE`, `FICLONERANGE`) | Not supported. |
| `mknod(2)` for device, FIFO, socket nodes | Not supported. The `beamfs_dir_inode_operations` table does not register a `.mknod` callback, so `mknod(2)` returns `-EPERM`. Non-blocking for typical Linux deployments where `/dev` is served by `devtmpfs` and named pipes / sockets are created on tmpfs or other writable mounts. Also affects `mkfs.beamfs --from-dir` which skips non-regular, non-directory, non-symlink entries from the source tree with a warning rather than attempting to recreate them on the target. |
| At-rest encryption | Not supported. `BEAMFS_FEATURE_INCOMPAT_ENCRYPT` (bit 12) is declared and reserved so an on-disk bit position exists before any volume could claim it by accident, but no cipher, key management, or format changes back it. If implemented, the intended composition order is encrypt then RS-encode then stamp DATA_CSUM / DATA_SELFID on write (mirrored on read), so integrity verification covers ciphertext and a failed check never reaches the decrypt step. Block-cipher diffusion means a single ciphertext bit flip can corrupt more plaintext than the RS correction budget was sized for under the current cleartext threat model; that budget would need re-evaluation before this bit is implemented, not silently inherited. See `beamfs.h` for the full rationale comment. |

---

## 5. Test coverage limitations

### 5.1 xfstests

**Coverage.** A full sweep runs from the `beamfs-xfstests` harness.
2026-09-18, 734 tests, one verdict each: 727 passed. Of the seven
that did not, `generic/589` is an upstream test defect (below),
`generic/074` was cut by the harness budget mid-write and its
findings are the interruption rather than a result, and five are
real: 075, 083, 241, and the two counted under 083's mechanisms.

Which mode a sweep ran in is on its own `MKFS_OPTIONS` line, and
must be read there before its rate is quoted. The sweep of
2026-09-21 (1789974744) ran with `-N 16384` alone, so with the mkfs
default, rs: its generic/001 alone logged thirty indirect-parity
checks, which a volume formatted `-I none` has no region to make.
Item 3.13 shows the two modes fail at very different rates, so a
rate from one says nothing about the other.

**The suite is 1016 commits behind upstream.** The recipe pins
xfstests 2024.03.03 (`SRCREV 088e5bd4`) against a 7.3.0-rc2 kernel.
`generic/589` failed identically on beamfs and on ext4 -- run as a
control -- because the test itself needed `TEST_DIR` to be shared
and did not say so until upstream `b53217a88c05` (Dave Chinner,
2024-11-27). That commit is backported in the layer's bbappend.
Every other verdict in the campaign carries the same doubt until
the suite is moved forward: a test that is wrong about beamfs looks
exactly like a beamfs defect.

**Control runs matter.** Running a failing test against ext4 in the
same harness cost minutes and saved a day of chasing a defect that
was not ours. It is worth making routine.

### 5.2 Fault injection

There is no automated fault-injection harness for beamfs in tree.
The only on-disk corruption testing performed to date has been
manual (single bit flips in the bitmap block, observed mount-time
correction). The threat model requires demonstrated coverage of:

- single-symbol errors in each protected structure
- multi-symbol errors at and just below the RS correction limit
- uncorrectable errors (verified to fail closed, not silently)
- burst errors crossing sub-block boundaries
- corruption events on metadata structures (superblock, inode
  table, bitmap, directory blocks)

A test framework providing reproducible injection of these
scenarios is required for the project to claim coverage of the
threat model. None exists at the time of writing.

### 5.3 Fuzzing

No targeted fuzzing has been performed on beamfs to date. Both
syzkaller (kernel module syscall surface) and afl++ (mkfs and
mount-time parsing of corrupted images) are appropriate tools and
are part of the planned offensive-security review stage.

---

## 6. Tooling and infrastructure

These items are not part of the kernel module itself but affect
reproducibility, integration, and operational use.

### 6.1 Mkfs / kernel parity validation

There is no automated test that compares, byte for byte, the
parity bytes produced by `mkfs.beamfs`'s embedded RS encoder
against those produced by the kernel's `lib/reed_solomon` for the
same input. A one-time manual validation has been performed and
recorded in `Documentation/testing.md` section "RS FEC Parity
Validation". An automated regression test should be added to the
xfstests recipe or to a dedicated tooling test suite.

### 6.2 `linux-mainline.cfg` provenance

The kernel configuration fragment used to build the test kernel
without KASAN/UBSAN noise is currently untracked. Its location and
versioning status need to be resolved: either it becomes part of
the layer (tracked, with a clear rationale documented for the
KASAN/UBSAN exclusion in the test path), or it is removed and the
default mainline configuration is used.

### 6.3 Cross-project file separation

A directory at `/tmp/bug-bounty-stash/` on the development host
contains kernel research files unrelated to beamfs. These were
adjacent during earlier development sessions but are out of scope
for this project. Their relocation to a separate repository (e.g.,
a local `bug-bounty-rdma` tree) is pending.

### 6.4 `bin/hpc-benchmark.sh` robustness under sudo

The HPC benchmark script in the yocto-beamfs repository uses
`~/.ssh/hpclab_admin` for SSH key resolution. When invoked under
`sudo`, `~` resolves to `/root` and the key path becomes invalid.
The current workaround is to call `sudo -v` first and run the
script as the regular user. The script should be modified to use
an absolute path or to derive the path from the invoking user's
home directory explicitly.

### 6.5 Yocto layer items

The following are tracked in the yocto-beamfs repository and
affect upstream readiness for `meta-openembedded` submission:

- `yocto-check-layer` does not yet PASS cleanly on the
  `arm64-beamfs` branch.
- Several HPC recipes lack `LIC_FILES_CHKSUM` and `HOMEPAGE`
  fields required by the layer index.
- Local patches against GCC 15 and QEMU are carried in the layer;
  these should either be removed (if the upstream issues are
  fixed) or proposed upstream to OE-Core.
- No one-shot bootstrap script exists that performs clone, build,
  and benchmark in a reproducible sequence. This is required for
  external reviewers to reproduce results without a multi-page
  setup procedure.

### 6.6 Build and test prerequisites (yocto-beamfs layer)

Two artefacts are required by the yocto-beamfs image recipes for
the HPC benchmark to start successfully but are NOT tracked by git:

  * `recipes-core/images/files/munge.key`     -- 1024-byte random
                                                 secret used by MUNGE
                                                 for inter-node Slurm
                                                 authentication.
  * `recipes-core/images/files/hpclab_admin.pub` -- SSH public key
                                                    matching
                                                    `~/.ssh/hpclab_admin`
                                                    on the host
                                                    workstation.

Both files have to be generated locally before the first build:

```sh
# munge.key
cd ~/git/yocto-beamfs/recipes-core/images/files/
dd if=/dev/urandom of=munge.key bs=1 count=1024 status=none
chmod 0400 munge.key

# hpclab_admin.pub (derived from existing private key)
ssh-keygen -y -f ~/.ssh/hpclab_admin > \
    ~/git/yocto-beamfs/recipes-core/images/files/hpclab_admin.pub
```

If either file is missing, the corresponding `ROOTFS_POSTPROCESS_COMMAND`
in the image recipe is silently skipped (the `[ -f ... ]` guard fails
without producing a Yocto error). The rootfs then ships without the
key, and `bin/hpc-benchmark.sh` fails at step 5 with a password prompt
(SSH) or at step 5/6 with a MUNGE key error (Slurm).

This is documented for context but the long-term fix is upstream in
the yocto-beamfs layer: either a one-shot bootstrap script that
generates the missing artefacts (related to known-limitation 6.5),
or a hard error in the recipe when the file is missing instead of
silent skip.

The full validation procedure including these prerequisites is
recorded in the project context document
`context-beamfs-validation.md`.

### 6.7 Commit signing policy

GPG commit signing is operational on the maintainer's development
host. Earlier sessions worked around a broken `pinentry-gnome3`
under Sway by passing `--no-gpg-sign` on every `git commit`. That
workaround is retired: `pinentry-curses` is now the active pinentry
binary (`/etc/eselect/pinentry`), explicitly declared in
`~/.gnupg/gpg-agent.conf`, and `GPG_TTY` is exported at login.

Policy from this point forward: every maintainer commit is signed
with GPG key `319A8EAA89C7538AA9550E8BC35EE212519E4857`. Tags are
annotated and may be additionally signed when the cryptographic
attestation toolchain (planned: Sigstore) is in place.

Commits made before this policy change carry no signature; they
remain valid and are not retroactively rewritten. The transition
point is the first signed commit on `main`.

## 7. Document maintenance

This document is reviewed at each release tag. The review consists
of:

1. For each item, determining whether it has been resolved,
   superseded, or remains valid.
2. Removing resolved items (their resolution being recorded in
   the relevant commit message and release notes).
3. Adding any new limitations identified since the previous review.
4. Reordering or restructuring sections only if the threat model
   itself has changed.

Editorial corrections (typos, formatting, dead links) do not
require this process and may be applied at any time.

---

## 8. Cross-references

| Document | Role |
|----------|------|
| `Documentation/threat-model.md` | Normative architectural constraints. Section 6 lists the constraints whose unmet status is recorded in section 2 of the present document. |
| `Documentation/roadmap.md` | Forward-looking work plan. Items in the present document are not roadmap items; the roadmap may or may not address a given limitation. |
| `Documentation/system-architecture.md` | Positioning of beamfs in the Linux storage stack and reference deployment scenarios. |
| `Documentation/design.md` | On-disk format specification. Limitations of the current format are reflected here. |
| `Documentation/testing.md` | Current test procedures and results. Section 5 of the present document complements `testing.md` with the test coverage gaps. |

### 3.26 fsck leaked its block-owner table on every clean check (fixed in fsck 0.1.6)

Valgrind on 2026-09-23, fsck.beamfs 0.1.5 checking a fresh 1 GiB volume:
1 949 608 bytes definitely lost in one block, allocated by
pass4_bitmap_rebuild. That is 243 701 data blocks times eight: the
owner_of table, freed on every error path and on the path that reports a
damaged bitmap, and not on the path a consistent bitmap takes -- the one
every healthy volume goes through. Harmless for a program that exits
right after, and still a bug: e2fsck runs clean under Valgrind, and a
checker that leaks on its own happy path is not held to the bar it
holds the filesystem to.

fsck 0.1.6 frees the table there too, and both tools gain
`make check-valgrind`: mkfs then fsck against a 1 GiB image under
memcheck, every leak kind an error. mkfs.beamfs 0.1.3 was already clean.

### 3.27 A parity slot written, then overwritten by a neighbour's stale region (fixed in 0.1.11)

generic/476 on module 0.1.10 (7.3-rc4) passed, unmounted cleanly, and
fsck found 102 indirect blocks holding pointers under a parity slot still
zero. parity.bt (beamfs-xfstests 2.3.29) recorded, for every one of
them, the last `beamfs_parity_slot` event with exactly the pointer count
fsck read from the block, and a write of the region block to the device
*after* that update. Not one region was left unwritten, not one update
postdated its region's last write: the update ran, the region went down,
and the bytes it carried were from before the update.

The mechanism is the per-cpu decode cache of the region (indparity.c, `ind_rcache`):
`beamfs_ind_parity_update` bumped `s_ind_parity_gen` *after*
`unlock_buffer`. In the window between the two, an update of another
slot in the same region on another cpu took the lock, found its cached
decode still stamped with the current generation, hit, scattered its own
slot into that pre-update payload, re-encoded and wrote the region. The
first slot had reached the device once and was erased by the second
writer. Fourteen slots share a region and fsstress writes many files at
once; 102 of 14 501 indirect blocks lost their parity this way in one
run, none of them visibly, since a zero slot reads as "not yet written"
and the block is walked unchecked.

0.1.11 bumps the generation before dropping the lock. The next holder of
the lock reads the new generation, misses, and decodes the buffer it
holds. ext4 has no such window because a metadata block's checksum lives
in the block itself and is written with it; a parity kept apart from what
it describes has to be kept coherent by hand, and this was the one hand
that let go too early.

Open, from the same run: fsck reports these blocks and does not count
them as damage (exit 0). Under the ext4 model a metadata block without
its checksum is corruption; fsck.beamfs should exit 4 on them.

### 3.28 Buffers that go back to what the medium held (open; diagnostics in 0.1.12)

generic/476 on 0.1.11, scratch image kept by beamfs-xfstests: 97 indirect
blocks under a zero parity slot, 43 of them under a region block that is
4096 bytes of zeros on the medium -- the state mkfs left -- although the
kernel had updated the slot in that region's buffer and the region had
been written to the device afterwards (parity.bt 2.3.29). In the same
run treecheck caught an installed pointer reading back as 0 from an
indirect block ("indirect slot lost its pointer", 13 blocks lost). Two
kinds of metadata buffer, one symptom: the bytes went back to what the
device held.

The kernel's own documentation (fs/buffer.c at clean_bdev_aliases, the
mapping_metadata_bhs series of 2026) names the one mechanism that does
this: a folio removed from the block device's mapping while a buffer on
it is still held. The holder keeps writing into an orphan; the next
reader gets a fresh folio from the device; whichever is written last
wins. 0.1.12 asks that question at the points of detection
(`beamfs_bh_diag`: the cache's buffer for the block, its folio, and
whether the mapping still holds that folio at the block's index) and on
every parity update (`beamfs_bh_attached`, a pointer compare).

Also recorded from the same reading: the mapping_metadata_bhs series
assumes metadata buffers are not shared between inodes; beamfs shares
its bitmap blocks and its parity regions (14 indirect blocks, up to 14
inodes) across inode lists. And beamfs keeps a dirty buffer-cache alias
of every data block it allocates (the zero image) beside the iomap page
that carries the data, where ext4 calls clean_bdev_aliases() to have no
alias at all.

### 3.29 The parity update reads its slot back (0.1.13, diagnostic)

0.1.12's beamfs_bh_diag answered at the point of detection: the region
buffer verify finds without a slot is the cache's own folio (`same`),
uptodate, clean, on no inode list -- not an orphan. parity.bt 2.3.33
(complete capture, maps printed first) then read b_data at every
mark_buffer_dirty of a region and at every write: for the undescribed
blocks the slot is absent already at the dirtying that follows their
own update; and for 30% of the healthy blocks too, uniformly over the 14
slots, although fsck reads those slots on the medium. One of the two
witnesses is wrong. 0.1.13 makes the module the arbiter: after
ind_region_write the slot is gathered back from b_data and compared with
what was scattered in; mismatches are said, and the running count is
printed every 4096 updates.

### 3.30 The slot read back after unlock_buffer (0.1.14, diagnostic)

0.1.13: 401 408 updates, 0 slots missing from b_data right after
ind_region_write, under the buffer lock. parity.bt reads b_data at the
mark_buffer_dirty that follows, after unlock_buffer, and finds the slot
absent for 30% of updates. 0.1.14 reads it back at that same instant
from inside the module. Present every time: the probe is wrong. Absent
sometimes: the bytes change in the window between unlock_buffer and
mmb_mark_buffer_dirty, and whoever takes the lock there is the writer
to find.

### 3.31 b_data against folio + bh_offset (0.1.15, diagnostic)

0.1.14: 401 408 updates, 0 slots gone from b_data after unlock_buffer.
parity.bt's 30% of absent slots were the probe's, not the module's; the
probe is withdrawn as a witness. What stands, from the module and the
image: the slot is in b_data at the dirtying, the region is written to
the device afterwards, the medium holds zeros, and at the next verify
the buffer is clean, on the mapping's own folio, without the slot. So
the write carried bytes other than b_data's. The device reads
folio_address(b_folio) + bh_offset(bh); the module writes b_data. 0.1.15
compares the two on every parity update, for the region and the
indirect block, and says the folio order when they differ.

### 3.32 The region decode cache, checked on every hit (0.1.16, diagnostic)

beamfs-xfstests 2.3.35 and 2.3.36 on 0.1.15, generic/476: every bio
that touches the region zone is the module's own one-block write; the
105 regions of the undescribed blocks are each written once, right
after their last update, never read again while mounted, never written
again, and hold zeros; and every lookup of a region block by number,
516 772 of them, comes from indparity.c. Nothing else finds these
buffers, so nothing else writes them. The one code that writes a
region's 4096 bytes is ind_region_write, from a scratch that is a fresh
decode of b_data or the per-cpu decode cache. The fresh decode cannot
regress the region; the cache can, and its key -- block number and
generation -- names no volume and no mount, while two beamfs volumes
are mounted during a run and the cache outlives them. 0.1.16 serves no
hit: it decodes every time and compares the decode with the entry,
counting hits, hits from another volume, and stale entries, and says
which devices were involved on each stale one.

### 3.33 The device read back after every region write (0.1.17, diagnostic)

0.1.16 served no cache hit and generic/476 still left 97 undescribed
indirect blocks: the region written is a fresh decode of b_data plus
the slot, and the medium holds zeros. With the slot in b_data, b_data
the bio's address, one write and no other bio, no other lookup, the one
thing never observed is the medium right after that write. 0.1.17
writes the region synchronously after each update, reads the block
back from the device past the page cache, and compares byte for byte;
buffers re-dirtied by another cpu in between are counted as skipped.
Identical every time: the zeros arrive later, with no write the block
layer sees. Different at once: the write path or the device.

### 3.34 The device read back under one hold of the lock (0.1.18, diagnostic)

0.1.17 on generic/476: 389 000 region writes followed by a raw read of
the device, 2416 of them coming back different, 50 all zero, no I/O
error. sync_dirty_buffer drops the lock before the read-back, so a
neighbour's update between the two makes b_data differ for a good
reason. 0.1.18 copies the bytes and submits the write under one hold
of the lock, reads the device back, and compares with the copy; a
difference is classified as the image from before this update (the
write did not land), all zeros, or other bytes, with the count of
differing bytes and the first offset.

### 3.35 A slot written this mount, checked at every later update (0.1.19, diagnostic)

The frozen image of generic/476 on 2026-09-25, read at the place the
kernel uses: 61 undescribed blocks sit in regions that hold slots
written later by neighbours and not theirs; 48 in regions entirely
zero. Every write was read back identical (0.1.18); no bio, read or
lookup touched the region between a slot's write and the neighbour's
update (2.3.35, 2.3.36, 2.3.38). The buffer's bytes lost the slot in
memory and the neighbour's fresh decode carried the loss down. 0.1.19
keeps, per region, the map of slots this mount wrote, checks every one
of them in the fresh decode at each update, says a loss at once with
the buffer's state, and reads the device directly to say whether it
still holds the slot.

### 3.36 The slot audit remembers only slots written non-zero (0.1.20)

0.1.19 reported 51 500 "lost" slots in one generic/476, most of them
the same slot at every later update of its region: a freed indirect
block has its parity recomputed over an empty block, and the parity
of zeros is zeros. A slot written all zero now clears its bit. What
remains reported is a slot written non-zero and found zero later.

### 3.37 Diagnostics withdrawn, 3.28 left open with its file (0.1.21)

0.1.12 to 0.1.20 were measurement builds. What they established, each
by a measurement and not by reasoning: the slot is in b_data at the
update (0.1.13, 0.1.14); b_data is the address the bio carries
(0.1.15); the decode cache never serves a stale region (0.1.16); the
device returns exactly what was written, read back past the page cache
under the lock (0.1.18, 405 465 of 405 504, the rest a neighbour's
update in the window); no slot written non-zero is ever missing from a
later fresh decode of its region (0.1.20, 196 608 checks); every bio
touching the region zone is a one-block write and nothing else looks
those buffers up (beamfs-xfstests 2.3.35, 2.3.36); the bare device
keeps 16 million random writes (2.3.41); fsck reads the same zeros
through the page cache, after flushbufs, past the page cache, and in
the host's backing file (2.3.43, 2.3.44). About a hundred indirect
blocks per generic/476 still end with an empty parity slot on the
medium, and no witness has yet seen the bytes change. The code goes
back to 0.1.11, which keeps the one real fix (the generation bumped
under the lock). fsck.beamfs 0.1.7 counts those blocks as damage, so
generic/476 fails the checker until this is closed.
