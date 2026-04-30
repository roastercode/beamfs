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
| 6.1 Universal data block protection (no opt-in) | Not implemented. RS FEC currently protects the on-disk allocation bitmap and, optionally, individual inodes flagged with `BEAMFS_INODE_FL_RS_ENABLED`. Data blocks themselves are not protected. |
| 6.2 Burst tolerance through stripe geometry | Not implemented. The bitmap uses 16 RS(255,239) sub-blocks packed within a single 4 KiB block, with no cross-block parity distribution. A burst exceeding 8 symbols within one 256-byte sub-block is uncorrectable in the current design. |
| 6.3 Unconditional inode RS protection | Implemented in stage 3 (v0.3.0+). All inodes are RS-protected unconditionally under `s_data_protection_scheme = INODE_UNIVERSAL`. The legacy `BEAMFS_INODE_FL_RS_ENABLED` flag is preserved in the bit definition for backward compatibility but is no longer functional. |
| 6.3 Superblock RS correction | Implemented in stage 3 item 2 (v0.3.0+). The superblock is protected by both CRC32 (detection) and RS(255,239) shortened over 8 sub-blocks of 211 data bytes (correction). On mount, a CRC32 mismatch triggers `beamfs_rs_decode_region()` over the staging buffer, with parity at offset 3968 of the superblock block. Recovery succeeds on up to 8 byte errors per sub-block. The corrected superblock is persisted to disk on the next metadata mutation via `beamfs_dirty_super()`, which encodes RS parity then recomputes CRC32 in that order. See `Documentation/design.md`, sections "Superblock CRC32" and "Superblock RS FEC". |
| 6.4 Shannon entropy in RS journal | Not implemented. The `beamfs_rs_event` structure records block number, timestamp, error symbol count, and per-entry CRC32, but not an entropy estimate. Distinguishing Family A (Poisson background) from Family B (correlated burst) post-event is therefore presently inferential rather than recorded.|
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
consequence, the Radiation Event Journal currently does not log
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

---

## 4. Filesystem feature limitations

These are deliberate scope restrictions of the current
implementation, documented for clarity. They are not defects in the
sense that the implementation matches its current specification;
they are gaps relative to a fully POSIX-compliant general-purpose
filesystem, which beamfs does not currently aim to be.

| Feature | Status |
|---------|--------|
| Maximum file size | Approximately 2 MiB (12 direct blocks + 1 single indirect block of 512 pointers). Double and triple indirect are declared in the inode but not implemented. |
| Symbolic links | Not supported. |
| Extended attributes (xattr) | Not supported. |
| SELinux labels (`security.selinux` xattr) | Not supported. |
| POSIX ACLs (`system.posix_acl_*` xattr) | Not supported. |
| `RENAME_EXCHANGE` flag | Returns `-EINVAL`. |
| `RENAME_WHITEOUT` flag | Returns `-EINVAL`. |
| `SB_RDONLY` enforcement | Not enforced. Writes to the superblock buffer occur on RS journal updates and bitmap writeback even if the filesystem was mounted read-only. |
| Online filesystem resize (grow/shrink) | Not supported. |
| Quotas | Not supported. |
| Reflinks (`FICLONE`, `FICLONERANGE`) | Not supported. |

---

## 5. Test coverage limitations

### 5.1 xfstests

Four `generic/*` tests have been validated: 002, 010, 098, 257.

`generic/001` requires a test image larger than 2 GiB to run to
completion (the test creates 200 sequential copies of approximately
3 MiB). On the current 512 MiB QEMU test image this exceeds the
available space and the test exits early. This is an environment
limit, not a defect in beamfs handling of the test workload.

A full xfstests Yocto recipe with a sized scratch image is not yet
in place. The four passing tests are run manually with a documented
procedure (`Documentation/testing.md`).

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

The HPC benchmark script in the yocto-hardened repository uses
`~/.ssh/hpclab_admin` for SSH key resolution. When invoked under
`sudo`, `~` resolves to `/root` and the key path becomes invalid.
The current workaround is to call `sudo -v` first and run the
script as the regular user. The script should be modified to use
an absolute path or to derive the path from the invoking user's
home directory explicitly.

### 6.5 Yocto layer items

The following are tracked in the yocto-hardened repository and
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

### 6.6 Build and test prerequisites (yocto-hardened layer)

Two artefacts are required by the yocto-hardened image recipes for
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
cd ~/git/yocto-hardened/recipes-core/images/files/
dd if=/dev/urandom of=munge.key bs=1 count=1024 status=none
chmod 0400 munge.key

# hpclab_admin.pub (derived from existing private key)
ssh-keygen -y -f ~/.ssh/hpclab_admin > \
    ~/git/yocto-hardened/recipes-core/images/files/hpclab_admin.pub
```

If either file is missing, the corresponding `ROOTFS_POSTPROCESS_COMMAND`
in the image recipe is silently skipped (the `[ -f ... ]` guard fails
without producing a Yocto error). The rootfs then ships without the
key, and `bin/hpc-benchmark.sh` fails at step 5 with a password prompt
(SSH) or at step 5/6 with a MUNGE key error (Slurm).

This is documented for context but the long-term fix is upstream in
the yocto-hardened layer: either a one-shot bootstrap script that
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
