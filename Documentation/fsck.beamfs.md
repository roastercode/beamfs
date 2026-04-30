# fsck.beamfs - Offline filesystem checker for beamfs

**Status**: design document, MVP under construction.
**Last updated**: 2026-04-30.
**Branch**: `mainline-prep`.

---

## 1. Position relative to beamfs online recovery

beamfs provides **online**, **synchronous** Reed-Solomon error correction
on the read path inside the kernel module. This is the steady-state
mechanism for surviving Single-Event Upsets (SEU), Multi-Bit Upsets (MBU),
and adversarial electromagnetic perturbations as defined in the EMR
threat model (`Documentation/threat-model.md`, sections 4 and 6).

`fsck.beamfs` does **not** replace this mechanism. It is the **offline
forensic recovery tool** that complements online RS, in three specific
situations where the kernel cannot or should not act:

1. **Post-FS_PANIC recovery**: when the kernel module has aborted the
   mount due to an uncorrectable error or saturation event, the volume
   is unreadable through normal mount. `fsck.beamfs` operates on the
   block device directly and may decode and rewrite the affected
   structures.

2. **Pre-mount validation**: before mounting a long-stored or potentially
   compromised volume, `fsck.beamfs --check-only` confirms structural
   integrity without modification, suitable for forensic chain-of-custody
   contexts.

3. **Post-event audit**: after a known electromagnetic event (e.g.
   confirmed solar flare, IEMI test, lightning strike), `fsck.beamfs`
   walks the entire on-disk structure and reports the full inventory of
   correctable, uncorrectable, and inconsistent regions, complementing
   the in-kernel RS event journal.

This positioning is consistent with `Documentation/threat-model.md`
section 6, which classifies offline tools as "complementing beamfs, not
replacing it".

---

## 2. Out of scope

`fsck.beamfs` does NOT:

- replace online RS correction during normal operation;
- run as a daemon or background service;
- perform online repair on a mounted volume;
- attempt cryptographic verification (that is dm-verity's job);
- attempt journal replay in the ext4/jbd2 sense (beamfs has no
  metadata journal in v4; the RS journal is an event log, not a
  redo journal);
- attempt fault injection or testing (that is RadFI's job);
- modify the volume unless explicitly invoked with `--repair`.

---

## 3. Operations

### 3.1 Read-only check (default mode, `--check-only`)

Walks the on-disk structure top-down without writing. Validates:

- block 0 superblock: magic, CRC32, RS(255,239)×13 over the coverage
  region (offsets 0..2742), parity at offset 3888..4095;
- block 1 inode-table: per-inode CRC32 + RS(255,239)×1 over
  i_reserved[0..15];
- block 2 bitmap: RS(255,239)×16 over the 16 sub-blocks of 239 bytes;
- block 5 first data block: scheme-dependent integrity walk
  (UNIVERSAL_INLINE = RS(255,239)×16 per block, parity tail-stored);
- RS journal: 64 entries, per-entry CRC32, monotonic timestamps,
  block-number sentinel respected.

Exit codes follow `fsck(8)` convention:

| Code | Meaning |
|------|---------|
| 0    | No errors |
| 1    | Errors corrected (after `--repair`) |
| 2    | System should reboot (not used by beamfs) |
| 4    | Errors left uncorrected |
| 8    | Operational error (cannot open device, etc.) |
| 16   | Usage or syntax error |
| 32   | fsck canceled by user |
| 128  | Shared library error |

### 3.2 Repair mode (`--repair`)

Same passes as check-only, but for each correctable region:

- if RS-decode succeeds, the corrected bytes are written back to disk;
- if a CRC32 mismatch persists after RS-decode, the region is flagged
  uncorrectable and recorded in the post-fsck audit log (no destructive
  action);
- if the bitmap is found inconsistent with the inode-table walk
  (block referenced by an inode but not marked allocated, or marked
  allocated but unreferenced), the bitmap is regenerated from the
  inode-table state, and the divergence is logged.

### 3.3 Force mode (`--force`)

Disables sanity checks that would normally abort fsck early (e.g. a
superblock magic mismatch). Intended for advanced forensic recovery
where the operator accepts the risk.

---

## 4. Mainline conformance contract

`fsck.beamfs` is structured to match Linux kernel.org filesystem
tooling conventions:

- written in C (not Rust), targeting POSIX with no external libraries
  beyond libc;
- the Reed-Solomon decoder is a copy of `lib/reed_solomon/` from the
  Linux kernel tree, adapted for userland (single-file, GPL-2.0). This
  guarantees byte-exact compatibility with the in-kernel decoder used
  by the beamfs module;
- exit codes follow `fsck(8)` convention (see section 3.1);
- standard error messages on `stderr`, progress on `stdout` only when
  invoked with `--verbose`;
- a manpage `fsck.beamfs.8` accompanies the binary;
- the binary integrates with the `fsck` wrapper via the `fs.beamfs`
  filesystem type registration.

---

## 5. Test integration

`fsck.beamfs` is the userland half of the `beamfs-bench fsck`
subcommand (Test D in the beamfs-bench TODO). Each Test D run:

1. Mounts a known-good beamfs volume.
2. Injects a fault sequence using RadFI that triggers FS_PANIC.
3. Forces unmount.
4. Invokes `fsck.beamfs --check-only` then `fsck.beamfs --repair`.
5. Re-mounts.
6. Verifies user data via SHA-256 against the pre-attack baseline.

Verdicts: `RECOVERED`, `RECOVERED_AFTER_FSCK`, `FSCK_NEEDED`,
`UNCORRECTABLE`.

---

## 6. Implementation plan

| Phase | Scope | Effort | Status |
|-------|-------|--------|--------|
| 0 | Scope document (this file) | 1h | DONE |
| 1 | Skeleton: Makefile + main.c + arg parsing | 2h | PENDING |
| 2 | Pass 1 - Superblock check + RS-decode | 4-6h | PENDING |
| 3 | Pass 2 - Bitmap check + RS-decode | 4-6h | PENDING |
| 4 | Pass 3 - Inode table walk | 6-8h | PENDING |
| 5 | Pass 4 - bitmap rebuild from inode-table | 8-12h | PENDING |
| 6 | Pass 5 - RS journal validation | 4-6h | PENDING |
| 7 | Manpage `fsck.beamfs.8` | 2h | PENDING |
| 8 | Integration `beamfs-bench fsck` Test D | 2-3h | PENDING |

---

## 7. References

- `Documentation/format-v4.md` - on-disk format reference.
- `Documentation/threat-model.md` - sections 4, 6 (offline tool position).
- `Documentation/roadmap.md` - Stage 4 (data block protection) close.
- `lib/reed_solomon/` upstream Linux kernel - RS decoder implementation
  to be vendored.
- `e2fsprogs/e2fsck/` - reference for fsck CLI conventions.
- `btrfs-progs/btrfs-check/` - reference for offline checker structure
  in a non-journaled FS.
- `xfs_repair` - reference for offline repair patterns.

