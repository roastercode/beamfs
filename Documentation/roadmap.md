# beamfs Roadmap

This document is the staged plan for beamfs development. It complements
two normative references and does not override them:

- `Documentation/threat-model.md` (section 6) defines the architectural
  constraints. Items in this roadmap that derive from a threat-model
  constraint cite the constraint number explicitly.
- `Documentation/known-limitations.md` records the present gap between
  the threat model and the implementation. As stages close, items move
  out of known-limitations and into the "what is done" record below.

Stages are sequenced so that each one's exit conditions are the
prerequisites of the next. Closing a stage requires the validation
chain in `context-beamfs-validation.md` (build clean, manual functional
test, HPC benchmark within the 20% regression policy, dmesg clean on
all four cluster nodes) plus the stage-specific sanity tests.

---

## Status table

| Stage | Title                                        | Status                  | Tag                       |
|-------|----------------------------------------------|-------------------------|---------------------------|
| 1.5   | On-disk bitmap with RS FEC (v2 format)       | CLOSED 2026-04-17       | (pre-tagging)             |
| 2     | Format extension points (v3 format)          | CLOSED 2026-04-26       | `v0.2.0-format-stable`    |
| 3     | Metadata hardening                           | CLOSED 2026-05-02       | (planned: `v0.3.0-metadata-hardening`) |
| 4     | Universal data block protection              | PENDING                 | (planned: `v0.4.0-*`)     |
| 5     | Offensive security analysis                  | PENDING                 | (planned: `v0.5.0-*`)     |

Tag naming convention: `vMAJOR.MINOR.PATCH-codename`. Each closing
stage produces an annotated GPG-signed git tag plus a Sigstore-signed
source tarball; the bundle is committed under `releases/`. See
`releases/README.md` for the verification procedure and the identity
table.

---

## Stage 1.5 - On-disk bitmap with RS FEC (v2 format)

**Status:** CLOSED 2026-04-17.

The bitmap block (block 5 in the default layout) stores the
free-block allocation state in 16 subblocks of 239 data bytes,
each protected by 16 bytes of RS(255,239) parity. The kernel
reads, verifies, and corrects the bitmap at mount time;
`mkfs.beamfs` encodes parity using the same GF arithmetic as
`lib/reed_solomon`, validated byte-for-byte against the kernel
encoder.

This is the first beamfs stage that meets the threat model
constraint 6.3 partially: the allocation bitmap is now correctable,
not only detectable. The superblock and inode table remain at
detection-only at the close of this stage.

Validated on arm64 QEMU kernel 7.0; benchmark reference run
2026-04-21 (3 metrics, zero RS errors, zero kernel BUG/WARN/Oops)
recorded as the canonical clean-host performance baseline in
`context-tir-de-performance.md` section 4.1.

Predates the Sigstore signing infrastructure; no release tarball.

---

## Stage 2 - Format extension points (v3 format)

**Status:** CLOSED 2026-04-26.
**Tag:** `v0.2.0-format-stable` (commit 56b28c8, GPG-signed annotated).
**Release tarball SHA-256:**
`19a196d18f9506c8f9ff149208ee03f2c9267aac674de3f7c1ba8c225a7684f2`.

Adds forward-compatibility infrastructure to the on-disk superblock
without changing the block layout:

- `s_feat_compat`, `s_feat_incompat`, `s_feat_ro_compat` (each
  `__le64`) for compat/incompat/RO-compat feature gating.
- `s_data_protection_scheme` (`__le32` enum) recording the
  data-block protection scheme the format was written with.
  v3 baseline value is `INODE_OPT_IN` (= 1), preserving v0.1.0
  behaviour.

`beamfs_crc32_sb` coverage extended to `[68, 1689)` so the new
fields are protected against single-bit corruption at the same
standard as the existing fields. v2 superblocks are explicitly
rejected by v3 kernels via the resulting CRC mismatch; the CRC is
the version barrier, no separate version-detection path.

Two latent build-time defects fixed in passing:

- `beamfs_crc32_sb` declared in `beamfs.h` since `fd371f3` and called
  from `super.c`, but never defined. Out-of-tree module builds
  failed at the link stage. Defined in commit `cdfe78b`. (Was
  known-limitations 3.6.)
- `encode_rs8` / `decode_rs8` in `lib/reed_solomon` take
  `uint8_t *data` on linux-mainline 7.0; the wrapper in `edac.c`
  was passing `uint16_t *`. Fixed in commit `867a911`. (Was
  known-limitations 3.7.)

Validated on the 4-node arm64 Slurm cluster on 2026-04-26: all 8
phases of `bin/hpc-benchmark.sh` (legacy, replaced by `beamfs-bench full`) passed, v3 mount on all 4 nodes,
distributed beamfs write via Slurm, zero BUG/WARN/Oops. 9-job
throughput +6.7% above the 2026-04-21 reference, within the 20%
regression policy. Latencies +69-83% due to a concurrent unrelated
syzkaller campaign on the host, documented in the release note.

Sigstore bundle committed at
`releases/beamfs-v0.2.0-format-stable.tar.gz.sigstore.json`.
Identity in the Fulcio cert: `aurelien.desbrieres@gmail.com`,
issuer `https://github.com/login/oauth`, validity 2026-04-26
00:08:01 - 00:18:01 UTC.

### Note: latent superblock writeback bug (discovered later)

A latent defect in the on-disk superblock writeback path,
present since the start of the project but masked by the
absence of mutation+remount test cycles, was discovered while
implementing stage 3 item 1 and resolved in commit ee6b6ae as
part of that work. It is recorded here under stage 2 because
the affected code (`beamfs_log_rs_event`, the four `alloc.c`
free-counter mutators) was already present at the v0.2.0
release. Two distinct symptoms:

  - `s_crc32` was not recomputed after any superblock mutation,
    so the on-disk superblock checksum drifted out of sync. The
    next remount failed with "superblock CRC32 mismatch".
  - `s_free_blocks` and `s_free_inodes` were updated in the
    in-memory `sbi->s_beamfs_sb` but never copied onto the
    buffer head before `mark_buffer_dirty(sbi->s_sbh)`. The
    counters were silently lost across umount/remount.

Resolution: a centralized helper `beamfs_dirty_super(sbi)` in
`super.c` propagates the in-memory authoritative copy onto the
buffer head, refreshes `s_crc32` via `beamfs_crc32_sb`, mirrors
the new `s_crc32` back to the in-memory copy, and marks the
buffer dirty. All five mutation sites (`beamfs_log_rs_event` +
4 alloc paths) call the helper instead of `mark_buffer_dirty`
directly. Validated runtime in qemuarm64: counters persist
across remount, and a remount after RS correction now
succeeds.

This trace is kept per MIL-STD-882E hazard-tracking practice:
every failure mode discovered during development is recorded
with its resolution, even when the resolution lands in a later
stage.

---

## Stage 3 - Metadata hardening

**Status:** ACTIVE.

This stage closes three normative gaps recorded in
`known-limitations.md` section 2 against threat-model section 6,
plus one behavioural defect from known-limitations 3.5. Each
gap is tracked as a numbered item with its own status; items
are sequenced so that earlier ones do not depend on later ones
but later ones may consume artefacts of earlier ones.

### Item 1 - Universal inode RS protection (CLOSED 2026-04-26)

**Threat model reference:** 6.1 (no opt-in), 6.3 (correction not
just detection) for the inode case.
**Commits:**
- beamfs `ee6b6ae beamfs: stage 3 item 1 -- universal inode RS protection (scheme 5)`
- yocto-hardened `aaa1cca beamfs: sync layer sources from beamfs HEAD ee6b6ae`

A new value `BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL = 5` is added
to the `s_data_protection_scheme` enum (`BEAMFS_DATA_PROTECTION_MAX`
bumped accordingly). mkfs writes scheme=5 going forward.
v0.1.0 / v0.2.0 images that carry scheme=1 INODE_OPT_IN remain
mountable on a stage-3 kernel; the kernel dispatches on the scheme
to decide whether the per-inode RS path is exercised. Block layout
unchanged from v3, no v4 format bump.

The legacy `BEAMFS_INODE_FL_RS_ENABLED` flag is preserved as a bit
definition for backward compatibility; the kernel no longer
consults it. The macros `BEAMFS_INODE_RS_DATA` (172) and
`BEAMFS_INODE_RS_PAR` (16) become live: parity goes into
`i_reserved[0..15]`, the rest of `i_reserved` is forced to zero.

Read path: CRC32 verification first, RS decode only on CRC fail
when scheme is `INODE_UNIVERSAL`. After successful correction the
kernel re-verifies CRC32 against the corrected buffer, logs the
event to the Electromagnetic Resilience Journal, writes the corrected inode
back to disk, and emits a `pr_warn`. This ordering avoids the
decoder on the fast path (>99 % of reads) and avoids the rare
risk of an SEU on the parity bytes inducing a false correction
on otherwise-valid data.

Write path: RS parity recomputed on every inode write after the
CRC32 field is set. Helpers `beamfs_rs_encode` and `beamfs_rs_decode`
in `edac.c` were generalized to accept a `size_t len` argument so
the same two functions serve the bitmap subblock path
(len = 239) and the inode path (len = 172). lib/reed_solomon
supports shortened RS codes natively.

Validated end-to-end on qemuarm64 with kernel 7.0: clean mount,
write/umount/remount cycle preserves counters, single-bit flip
on inode 2 triggers RS recovery + log event + corrected
writeback, subsequent remount succeeds, dmesg clean.

### Item 2 - Superblock RS correction (CLOSED 2026-04-26)

**Threat model reference:** 6.3 (correction not just detection)
for the superblock case.

**Commits:**
- beamfs `4227354` (commit A -- RS region helpers in edac.c)
- beamfs `874dfdd` (commit B -- mkfs SB RS layout)
- beamfs `584440e` (commit C -- kernel SB RS decode + dirty_super)
- yocto-hardened layer sync commits, lockstep

Implementation followed the 3-commit plan A/B/C unchanged from
the design below. v3 format preserved (parity placed in `s_pad`
at offset 3968, 8 RS subblocks of 16 bytes parity each).

The superblock today (v0.2.0+, including stage 3 item 1) has
CRC32 detection only; corruption causes mount failure. This item
adds Reed-Solomon FEC covering the same byte range as
`beamfs_crc32_sb` (`[0, 1689)`), so a corrupted superblock is
correctable in place at mount.

#### Design

- **RS coverage:** `[0, 1689)` (mirror of CRC32 coverage).
- **Subblock decomposition:** 8 RS(255,239) shortened codewords.
  Gives 64 symbols of correction capacity total (8 per codeword),
  strictly superior to a single shortened RS code on the full
  range (8 symbols total). Coherent with the existing bitmap
  pattern. MIL-STD-882E favourable: 8 independent
  failure-correctable regions vs 1 single point.
- **Parity placement:** offset 3968 in the 4096-byte superblock
  (last 128 bytes of `s_pad`, `4096 - 8 * 16 = 3968`). Chosen
  for stability against future format evolution: new fields go
  before the parity in `s_pad`.
- **No format bump:** layout stays v3. `s_pad` was already 2407
  bytes, the 128-byte parity fits with margin.
- **Backward compat:** v0.1.0 / v0.2.0 images mount cleanly
  because the parity zone (zeros on those images) is never
  consulted as long as CRC32 passes. On those images, a
  corruption breaking CRC32 still causes a mount failure as
  before -- no regression.

#### Implementation plan (3 atomic commits A/B/C)

- **Commit A -- RS region helpers in `edac.c`.**
  Factor the "encode N subblocks of len bytes from a buffer"
  pattern into two helpers: `beamfs_rs_encode_region(buf,
  region_len, subblock_count, parity_offset)` and
  `beamfs_rs_decode_region(...)`. The bitmap path in `alloc.c`
  is refactored to use them. No on-disk change, no runtime
  change on the bitmap (same RS calls, just extracted into a
  function). Test: bitmap functional unchanged.

- **Commit B -- Superblock RS layout, mkfs side.**
  Constants `BEAMFS_SB_RS_OFFSET = 3968` and
  `BEAMFS_SB_RS_SUBBLOCKS = 8` in `beamfs.h`. `mkfs.beamfs.c`
  encodes 8 RS subblocks on the superblock after `crc32_sb`
  and writes the parity at offset 3968. No kernel decode yet:
  mount continues to fail on CRC fail. Test: mkfs produces a
  superblock whose parity zone is no longer zero; mount on
  fresh image still OK (CRC32 valid, RS never consulted).

- **Commit C -- Stage 3 item 2 closure: kernel decode + dirty_super.**
  `beamfs_dirty_super` extended to encode RS parity on every
  superblock mutation (before the CRC32 recompute).
  `beamfs_fill_super` adds, after the CRC32 check, the RS
  recovery path: on CRC fail, decode RS, re-verify CRC, log
  RS event on block 0, mark dirty for writeback. Update
  `design.md`, `known-limitations.md` (KL 6.3 second half
  marked implemented), `README.md`. Tag
  `v0.3.0-metadata-hardening` expected **after** items 3
  and 4 are also closed.

### Item 3 - Fix `beamfs_rs_decode` return convention (CLOSED 2026-04-26)

**Reference:** known-limitations 3.5.

**Commits:**
- beamfs `54f500c beamfs: stage 3 item 3 -- fix beamfs_rs_decode return convention (KL 3.5)`
- yocto-hardened `451e0ad beamfs: sync layer sources from beamfs HEAD 54f500c`

Resolution applied per option (1) of KL 3.5: `beamfs_rs_decode` now
returns the symbol count on successful correction, keeps `-EBADMSG`
for uncorrectable. All callers (bitmap subblock loop in `alloc.c`,
inode RS path in `inode.c`) updated to test `rc > 0` for correction
events. The new contract is the prerequisite for item 4 (Shannon
entropy consumes the symbol count and the position list).

Today `beamfs_rs_decode` returns 0 (success, regardless of
whether corrections occurred) or `-EBADMSG` (uncorrectable).
The symbol count produced by `decode_rs8` is dropped. As a
consequence, the bitmap correction path in
`alloc.c::beamfs_setup_bitmap` tests `if (rc > 0)` to identify
a correction event -- that branch never matches under the
current decoder semantics.

Fix per option (1) of known-limitations 3.5: return the symbol
count on success, keep `-EBADMSG` for uncorrectable. Propagate
to all callers (bitmap subblock loop in `alloc.c`, inode RS
path in `inode.c`, superblock RS path from item 2). The new
return contract is the prerequisite for item 4 (entropy uses
the count).

### Item 4a - Refactor SB serialization (CLOSED 2026-04-26)

**Commits:**
- beamfs `c75a067 beamfs: refactor SB serialization to offsetof + BUILD_BUG_ON + static_assert`
- yocto-hardened `42a520f beamfs: sync layer sources from beamfs HEAD c75a067 + add invariants/validate scripts`

Refactored `beamfs_sb_serialize` and `beamfs_sb_deserialize` to use
`offsetof()` macros throughout, with `BUILD_BUG_ON` sentinels at
module init and `static_assert` at compile time guarding every
on-disk field offset and structure size. The sentinels are the
compile-time tripwire that will force every call-site through
verification when item 4 bumps `struct beamfs_rs_event` from 24
to 40 bytes. Added `bin/beamfs-invariants.sh` (4 static invariants)
and `bin/beamfs-validate.sh` (chains invariants + HPC bench).

### Item 4b-dirent - Dirent slot reuse fix + research deployment (CLOSED 2026-04-26)

**Commits:**
- beamfs `a7119c5 beamfs: fix directory entry slot reuse after deletion`
- beamfs `aaa18d5 doc/roadmap: track userspace tooling integration as upstream prerequisite`
- beamfs `85df057 doc/README: add 2026-04-26 HPC validation on research deployment`
- yocto-hardened `0e3f9f3 beamfs: sync dirent slot reuse fix from upstream + inv5 regression guard`
- yocto-hardened `8e3cac1 research image: hpc-arm64-research.bb + bench refactor for real /dev/vdb beamfs`
- yocto-hardened `f217af8 doc: 2026-04-26 baseline + README milestones for research deployment`

Dirent slot reuse bug: `beamfs_del_dirent` zeroed the full 268-byte
`struct beamfs_dir_entry` including `d_rec_len`, causing readdir
and lookup loops that advanced via `offset += le16_to_cpu(de->d_rec_len)`
and broke on `!d_rec_len` to treat a freed slot as end-of-block,
masking live entries after the hole in the same 4 KiB block.
Fix (option alpha): full-block scan, advance unconditionally by
`sizeof(struct beamfs_dir_entry)`, free slot identified by
`d_ino == 0`. Static invariant `inv5_dirent_no_break_on_zero`
added as pre-commit guard against regression.

Research deployment: new Yocto recipe `hpc-arm64-research.bb`
replaces `hpc-arm64-master.bb` + `hpc-arm64-compute.bb` with a
single squashfs read-only rootfs image (~52 MB), real `/dev/vdb`
beamfs partition (no more loopback), kernel cmdline-driven hostname,
`beamfs.ko` packaged in image (no manual injection). `hpc-benchmark.sh` (now superseded by `beamfs-bench full`)
phases 2-7 refactored. Architecture is now OIV-grade. Pre-v4 I/O
baseline captured on this architecture and recorded in
`Documentation/iobench-baseline-2026-04-26.md` (yocto-hardened layer):
M1 write seq + fsync (4MB) Med 5.000 MB/s, M2 read seq cold (4MB)
Med 20.000 MB/s, M4 stat bulk (100 files) Med 0.150 s, M5 small
write + fsync (10x64B) Med 24.000 ms. This baseline is the
reference against which item 4 (v4 bump) will be compared
(regression criterion: delta < 10 % on M1+M2).

### Item 4 - Shannon entropy in RS journal (CLOSED 2026-05-02)

**Threat model reference:** 6.4 (tamper-evident journal).
Reclassified as Must-have by threat-model section 8 §1.

Each `beamfs_rs_event` records a per-correction entropy estimate.
Entropy is the forensic discriminator between Family A (Poisson
background) and Family B (correlated burst), computed on the
symbol position list returned by item 3's extended return contract.

#### Decision: format bump v3 -> v4

The placement evaluation reached the conclusion that no in-place
reuse of existing 24-byte fields is acceptable: entropy needs a
dedicated `__le32` field plus a flags byte plus a CRC, and the
event entry needs to grow to 40 bytes. v4 is therefore a strict
format bump (kernel item-4 refuses to mount image v3).

#### Target struct (40 bytes, packed)

```
struct beamfs_rs_event {
    __le64  re_block_no;          /*  0..7   */
    __le64  re_timestamp;         /*  8..15  */
    __le32  re_symbol_count;      /* 16..19  renamed from re_error_bits */
    __le32  re_entropy_q16_16;    /* 20..23  Shannon H, Q16.16, range [0,3] */
    __le32  re_flags;             /* 24..27  bit 0 = entropy_valid */
    __le32  re_reserved;          /* 28..31  zero, structural sentinel */
    __le32  re_crc32;             /* 32..35  CRC32 over bytes 0..31 */
    __le32  re_pad;               /* 36..39  zero, alignment + sentinel */
} __packed;
```

#### Target SB layout v4 (13 subblocks)

The growth of `struct beamfs_rs_event` propagates to the journal
capacity sizing in the superblock, requiring SB layout adjustment:

- `BEAMFS_SB_RS_COVERAGE_BYTES`  : 1685 -> 2709
- `BEAMFS_SB_RS_STAGING_BYTES`   : 1688 -> 2743 (13 * 211)
- `BEAMFS_SB_RS_DATA_LEN`        : 211 (unchanged)
- `BEAMFS_SB_RS_SUBBLOCKS`       : 8 -> 13
- `BEAMFS_SB_RS_PARITY_BYTES`    : 128 -> 208
- `BEAMFS_SB_RS_PARITY_OFFSET`   : 3968 -> 3888
- `s_pad[]`                     : 2407 -> 1383

Arithmetic check: 2713 + 1175 + 208 = 4096.

#### Shannon entropy algorithm (binned, B=8, deterministic)

H = sum over 8 bins of LUT[N][bin_count[i]], where N is the
number of corrected positions, k_i is the count of positions in
bin i, and `LUT[N][k] = -k/N * log2(k/N)` precomputed in Q16.16.
The LUT is generated by an external Python script and hardcoded
in `edac.c`. No FPU, no runtime division, deterministic in cycles.
Range: H in [0, 3] bits (log2(8) = 3).

#### Implementation plan (6 sub-commits A-F)

- **Commit A** -- `beamfs.h`: struct `beamfs_rs_event` v4 (24 -> 40
  bytes), defines for SB layout v4, `BUILD_BUG_ON` sentinels.
- **Commit B** -- `edac.c`: LUT Q16.16, `beamfs_rs_compute_entropy`
  helper, new signature for `beamfs_rs_decode` /
  `beamfs_rs_decode_region` exposing position list.
- **Commit C** -- `super.c`: v4 mount path (strict reject != V4),
  `beamfs_log_rs_event` new signature with entropy, new call-site
  for SB recovery (the deferred slot from item 2 commit C).
- **Commit D** -- `inode.c`, `alloc.c`: propagate position buffer,
  compute entropy at call-sites.
- **Commit E** -- `mkfs.beamfs.c`: v4 sync (defines + struct
  mirrors + emit `s_version = 4`).
- **Commit F** -- documentation: `testing.md`, new
  `format-v4.md`, roadmap update, README HPC validation v4.

Tag `v0.3.0-metadata-hardening` follows after lockstep push of
all sub-commits and successful HPC validation against the pre-v4
baseline.

### Sanity tests for stage 3 closure

In addition to the standard validation chain
In addition to the standard validation chain
(`context-beamfs-validation.md` section 3):

- All inodes show RS-protected at mount (no flag-gated path
  remains).
- A deliberate single-bit flip on the superblock (via `dd` on a
  detached image) is corrected at mount and logged with a
  computed entropy estimate.
- A deliberate single-bit flip on an inode is corrected on the
  next read of that inode and logged.
- `beamfs_rs_decode` returns the correct symbol count on
  injected single-symbol and multi-symbol-but-correctable
  errors. Bitmap correction events appear in the journal
  (correctness 3.5 fix verified).
- HPC benchmark passes within 20% of the 2026-04-21 reference,
  on a clean host (no concurrent syzkaller).

### Format implication

Items 1, 2, 3 closed without format bump (v3 preserved). Item 4
requires a strict v4 bump as the entropy field cannot be hidden
in the existing 24-byte `beamfs_rs_event` footprint without
sacrificing the structural sentinels and CRC. Stage 3 closes on
format v4. `s_data_protection_scheme` remains at
`INODE_UNIVERSAL` (= 5) introduced by item 1.

### Tag

Planned: `v0.3.0-metadata-hardening` (or a name reflecting the
final layout choice). Annotated, GPG-signed, Sigstore-signed
under the same identity as `v0.1.0-baseline` and
`v0.2.0-format-stable`.

---

## Stage 4 - Universal data block protection

**Status:** PENDING.

This stage closes threat-model constraint 6.1 (universal data
block protection, no opt-in) and 6.2 (burst tolerance through
stripe geometry).

### Scheme selection

`s_data_protection_scheme` enum already reserves the three
candidate schemes (see `Documentation/design.md` section "Data
protection schemes"):

| Value | Scheme                                  | Trade-offs |
|-------|------------------------------------------|------------|
| 2     | `UNIVERSAL_INLINE`                       | RS parity inside each data block. Simplest. Fixed overhead per block. No protection against full-block loss. |
| 3     | `UNIVERSAL_SHADOW`                       | RS parity in a dedicated out-of-band region with stride placement. Burst-tolerant if stride > burst length. Layout cost. |
| 4     | `UNIVERSAL_EXTENT`                       | RS parity as an extent-attribute. Distribution pattern per extent. Most flexible, most complex. |

Stage 4 begins with a written comparison and a chosen scheme,
recorded in a new `Documentation/data-protection-design.md`. The
choice is constrained by the LOC budget of threat-model 6.5
(under 5000 lines auditable, current 2700, available margin
~2300).

### Exit conditions

1. All stage 3 sanity checks still pass.
2. Single-block data corruption (deliberate `dd` flip) is
   auto-corrected on read; correction logged.
3. Burst corruption across multiple consecutive blocks is
   handled per the chosen scheme's stated burst tolerance.
4. No data-block read bypasses RS verification (universal
   coverage, no flag-gated path).
5. Throughput documented in
   `~/git/yocto-hardened/Documentation/benchmark.md`. The
   write-path overhead is expected, the read-path overhead is
   expected to be small except on correction events.

### RFC v4 readiness - exit conditions of stage 4

Per `Documentation/roadmap.md` history (the previous "Must-have
before v4 submission" list, now incorporated here): RFC v4 to
linux-fsdevel cannot be submitted before stage 4 closes,
because reviewers (Pedro Falcato in particular) flagged the
opt-in model as a use-case scope problem. Submitting v4 with
stage 3 closed but stage 4 open would re-trigger the same NACK.

Therefore the following items are **conditions of stage 4
closure**, not separate Must-haves:

- xfstests Yocto recipe with `generic/{001,002,010,098,257}`
  passing inside Yocto. Currently four pass manually
  (002, 010, 098, 257); 001 needs a >2 GiB scratch image
  (known-limitations 5.1).
- Patch series restructured into a clean atomic series
  (rebased fixups, no surgery on already-reviewed patches).
- Response to all open reviewer comments staged in the cover
  letter draft. Eric Biggers' RS comments are already
  addressed and validated.

### Tag

Planned: `v0.4.0-universal-protection`. RFC v4 submission to
linux-fsdevel happens immediately after this tag.

### Empirical motivation N=100 (2026-05-10)

The N=100 multifs experiment run on 2026-05-10 (raw data tarball
sha256 `f9f01a585c2871cab063ffbf2bbfc015777537918339a5d742b2137846e8a615`)
provided three empirical observations that factualize the priority
of this stage :

1. **Theorem v2.2 corroboration on scheme=2 (UNIVERSAL_INLINE)**.
   At 1M ppm probability with `flip_locality=RANDOM`, scheme=2
   produced 60 `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE` journal entries
   across 100 runs, with mean `RS_CORRECTED=5.07` and 89% hash
   preservation. Theorem v2.1 (recovery within capacity) and
   Theorem v2.2 (saturation detected and flagged) are both
   empirically corroborated on the same dataset, without need
   for a dedicated saturation cell.

2. **Scheme=5 data block protection gap empirically measured**.
   Across 600 cells (100 runs × 6 probabilities) of scheme=5
   (INODE_UNIVERSAL), the cumulative `RS_CORRECTED` count is
   exactly **zero**. Cumulative `BITS_DIFF` reaches 92,005 bits
   over the same 600 cells. The decoder `beamfs_rs_decode_region`
   is never invoked on the data block read path because
   `file.c::generic_file_read_iter` (the path used for files
   exceeding `BEAMFS_DATA_INLINE_BYTES = 3824 bytes`) does not
   integrate the RS decode step. This is the gap stage 4
   addresses.

3. **Bench multifs calibration limitation observed**. The
   N=100 calibration used `target_block_range` configured for
   the beamfs file layout, producing FLIP_DELTA=0 across
   ext4/btrfs/xfs/f2fs/exfat/vfat/squashfs. The commodity FS
   results in that run are therefore **invalid for comparison**;
   they reflect the absence of injection on those FS, not their
   resilience. Equitable multifs comparison requires the
   emufi v0.4 multifs-capable injector tracked in the
   companion `~/git/emufi/Documentation/roadmap.md`.

These observations confirm that stage 4 is the empirically
critical next step. The N=100 dataset is preserved at
`/tmp/N100-20260510-003122/N100-raw-data.tar.gz` on the
research deployment and will be referenced in paper v3.

---

## Stage 5 - Offensive security analysis

**Status:** PENDING.

This stage produces `Documentation/security-analysis.md`. The
analysis IS the validation; per
`context-beamfs-validation.md` section 6.4, there is no separate
acceptance criterion beyond the document landing in tree under
review.

### Scope

- Targeted fuzzing with syzkaller against the beamfs module
  syscall surface (mount, file ops, ioctls if any), and afl++
  against `mkfs.beamfs` and the mount-time parsing of corrupted
  images.
- Fault-injection harness for reproducible coverage of the
  scenarios listed in known-limitations 5.2: single-symbol
  errors per protected structure, multi-symbol at and below
  the RS limit, uncorrectable (must fail closed), burst
  errors crossing sub-block boundaries, corruption events on
  metadata structures (superblock, inode table, bitmap,
  directory blocks).
- Adversarial review against the Family B threat actor
  profile of threat-model section 3.

### Exit conditions

1. `Documentation/security-analysis.md` exists, peer-reviewed
   by at least one external reviewer with kernel security
   background.
2. The fault-injection harness is in tree under
   `tools/beamfs-fuzz/` or equivalent and is invokable as a
   standalone command.
3. No High-severity finding remains open. Medium-severity
   findings are documented in known-limitations with mitigation
   timeline.

### Tag

Planned: `v0.5.0-security-reviewed`.

---

## Long-term vision (post-merge)

### Principal long-term objective: Full beamfs rootfs

The principal long-term engineering objective of the beamfs project
is to be deployable as a **full Linux rootfs**, replacing squashfs
on production OIV-grade systems where electromagnetic resilience is
a stated design constraint. This is the deployment scenario that
fully realizes the EMR threat model of `threat-model.md`: the
operating system itself, not merely its data partition, runs on a
filesystem that detects and corrects EMR perturbations autonomically.

Concretely: a Yocto image whose `IMAGE_FSTYPES = "beamfs"` (no
squashfs intermediate, no tmpfs overlay), boots from beamfs, runs
init from beamfs, mounts `/`, `/etc`, `/var`, `/home` all on the
same beamfs partition, and exhibits the autonomic repair contract
of Theorem v2.1 across the full operating-system surface, not just
on synthetic loop images.

This objective dominates the post-merge roadmap. The items below
(read-only mode with compression, extended attributes and SELinux,
post-quantum metadata authentication) are **prerequisites or
adjuncts** to the full-rootfs deployment, not parallel work.

#### Dependency chain

The full rootfs cannot be deployed until each of the following is
in tree, validated, and stable under load:

1. INLINE write path (single-block scope, `develop` branch as of
   2026-04-29). Required for any write operation on an INLINE
   volume.
2. Multi-block INLINE write path (v2.x roadmap). Required for any
   file > 3824 bytes, which includes essentially all configuration
   files, logs, and binaries.
3. Full POSIX semantics: `link`, `rename`, `truncate`, `fsync`,
   `mmap`, `xattr`, ACL. Each is a known-limitation today.
4. `xfstests` "auto" group passing under load, with the four-node
   HPC cluster as the test substrate.
5. Symlinks. Required for any rootfs use (numerous compatibility
   symlinks live in `/usr/lib`, `/etc`, `/var`).
6. Extended attributes and SELinux. Prerequisite for any
   mandatory-access-control deployment, which is a typical OIV
   requirement.
7. `dm-verity`-equivalent verified-boot story for beamfs, tying
   the filesystem to the trusted-boot chain. The RS FEC layer is
   already at the FS level; verified boot integrates the
   filesystem identity into the boot chain attestation.

Each of these is a separate engineering effort estimated in the
weeks-to-months range. The cumulative path from the present
session (INLINE write single-block on `develop`) to a tagged
`v3.0.0-full-rootfs` release is estimated at 6-12 months of focused
work, assuming the v2.x post-Zenodo continuation runs unblocked.

#### Publication anchor

A separate Technical Report (beamfs v3 - Full POSIX filesystem with
electromagnetic resilience: production deployment validation) is
planned to accompany the `v3.0.0-full-rootfs` engineering tag.
This report will document the deployment scenario, the
end-to-end empirical validation under load (boot, init, OS
operations, application workloads), and the recovery contract
generalized from synthetic Theorem v2.1 fixtures to the full
operating-system surface.

The objective is registered here as the canonical long-term
direction of the project. Subsequent design decisions
(format-v5 considerations, kernel API choices, userspace
tooling priorities) are evaluated against it.

---

The following items are out of the staged plan and are sketched
here only to make scope boundaries explicit relative to the
principal objective above. They are sequenced as adjuncts on the
path to full rootfs.

### Read-only mode with compression

A read-only mount mode combining RS FEC verification with
optional block-level compression (lz4, zstd), positioning beamfs
as an alternative to squashfs for radiation-exposed read-only
partitions: squashfs detects nothing, beamfs read-only mode
would detect and correct.

### Extended attributes and SELinux

`security.selinux` and POSIX ACL xattrs, prerequisite for
mandatory access control on beamfs partitions. Combined with the
read-only mode above and symlink support below, makes beamfs
suitable as a hardened Linux rootfs.

### Symlinks and rootfs capability

Required for any rootfs use. Combined with indirect blocks
(already implemented), xattr, SB_RDONLY enforcement
(known-limitations section 4), and the read-only mode, beamfs
becomes deployable as both the root and the data partition of
a hardened embedded Linux system.

### Post-quantum metadata authentication

Per-block hash-based signatures (NIST FIPS 205 SLH-DSA /
SPHINCS+) on the superblock and inode structures, providing
post-quantum authenticated integrity for environments where
the threat model includes an active write-capable adversary
on the storage medium. Out of scope for v1 and v2 per
threat-model 2.3.

---

## Userspace tooling integration (TODO)

For beamfs to be a first-class filesystem in the Linux ecosystem
and a credible candidate for upstream submission to kernel.org,
the userspace tooling layer needs work beyond the kernel module
and the standalone `mkfs.beamfs` binary. The following items are
tracked here as long-term tooling deliverables; none of them is
a blocker for current research deployment, but each one is
required for production-grade adoption.

### parted

`parted` does not currently know about beamfs as a filesystem
type. A partition formatted with beamfs shows up as `unknown`
in `parted print` and cannot be created with `mkpart` using a
fs-type argument of `beamfs`. Two approaches:

1. Submit a patch to GNU parted (`libparted/fs/`) registering
   beamfs as a known fs-type with its magic word and offset.
2. Until the patch lands upstream, ship a parted-beamfs.patch
   in this repository for downstream consumers to apply.

Option 1 is the long-term play and the one that gives beamfs
visibility in the GNU/Linux toolchain.

### util-linux (blkid, lsblk, wipefs)

`blkid` is the canonical mechanism by which userspace tools
(udev, mount, fstab, systemd, parted) identify the filesystem
on a block device. The signature lives in
`util-linux/libblkid/src/superblocks/`. A new
`superblocks/beamfs.c` is required, registering:

- The beamfs magic word `FTRF` at byte offset 0
- The version field for `format_version` reporting
- The UUID field for `UUID=` mount syntax
- The label field if/when beamfs gains volume labels

Once `blkid` recognises beamfs, `lsblk -f`, `mount UUID=...`,
`wipefs`, and udev rules all work transparently. This is the
single most impactful upstream patch for end-user adoption.

### fsck.beamfs

A repair tool that can:

- Walk the inode bitmap and the inode table to detect
  inconsistencies
- Verify CRC32 on every inode and superblock copy
- Run RS FEC decode on damaged regions and report which
  blocks were corrected
- Detect and repair the dirent slot reuse class of bugs
  (cross-reference with the testing.md regression test)
- Operate read-only by default; `--repair` only after
  explicit consent

`fsck.beamfs` is invoked automatically by `fsck` from
util-linux when the filesystem type is beamfs, provided the
binary is named `/sbin/fsck.beamfs` per the fsck(8) convention.

### mount(8) integration

`mount -t beamfs` already works because the kernel module
registers the filesystem type. What is missing:

- An `fstab(5)` entry pattern documented in this repo
- A udev rule to auto-mount beamfs partitions detected by
  blkid (once the blkid integration above lands)
- Cleaner error messages on mount failure (currently relies
  on dmesg)

### Yocto layer integration

`meta-yocto-hardened-hpc` already ships `beamfs-module`,
`mkfs-beamfs`, and `beamfsd`. Adding:

- A `wic` plugin or kickstart fragment so Yocto images can
  declare beamfs partitions natively in their `.wks` files
- A `dm-verity`-equivalent for beamfs (the RS FEC layer is
  already there at the FS level, but a verified-boot story
  would tie beamfs to the trusted-boot chain)

### Argument for kernel.org submission

When the patch series is submitted to fsdevel and linux-kernel,
one of the standard reviewer questions is *"is there userspace
tooling support? does blkid recognise it? does parted work?
is there an fsck?"*. Having concrete answers (`yes, patch
series N for util-linux, patch series M for parted, fsck.beamfs
in this tree`) is what separates a research POC from a
submittable filesystem. The work tracked here is the path from
one to the other.

---

## Publication roadmap

The project's engineering milestones are mirrored on a public
publication roadmap. Each major engineering tag triggers a new
version of the project's Technical Report, deposited on HAL with
the same `idHAL` (`aurelien-desbrieres`) for cumulative anteriority.

| Version | Engineering trigger                  | Status              | Zenodo DOI                                                          |
|---------|--------------------------------------|---------------------|---------------------------------------------------------------------|
| v1      | Stage 3 partial closure (this state) | Published           | [10.5281/zenodo.19824442](https://doi.org/10.5281/zenodo.19824442)  |
| v2      | `v0.3.0-metadata-hardening`          | Planned             | (TBA)                                                               |
| v3      | `v0.4.0-universal-protection`        | Planned             | (TBA)                                                               |
| v4      | `v0.5.0-security-reviewed`           | Planned             | (TBA)                                                               |

**Note on HAL.** A HAL deposit was attempted for v1 (`hal-05603801`)
but was declined on 2026-04-27. HAL's editorial policy restricts
deposits to authors affiliated with academic institutions, registered
PhD candidates, or holders of an official doctoral degree, with a
narrow exception for authors with an extensive publication record in
internationally peer-reviewed journals. The author's profile (15+
years of professional experience in critical-infrastructure Linux
engineering, Linux kernel mailing-list engagement, technical media
coverage) does not match these criteria. HAL itself recommended
Zenodo as the appropriate alternative. Zenodo (CERN, EU-funded) is
therefore the canonical citable identifier for this Technical Report
series.

The v1 source LaTeX, bibliography, dataset, and build instructions
are committed under [`papers/2026-04-beamfs-v1/`](../papers/2026-04-beamfs-v1/).
Each future version inherits the same directory layout
(`papers/YYYY-MM-beamfs-vN/`) for archival traceability. HAL native
versioning preserves all prior versions and timestamps each new
submission. Each version will additionally be mirrored to Zenodo
via a GitHub release tag, yielding a perennial DOI independent of
the HAL platform.

The cross-reference table above must be updated at each milestone
closure together with the engineering status table at the top of
this document. The update process is described in the Document
maintenance section below.

---

## Document maintenance

This roadmap is updated at each stage closure. The update
consists of:

1. Move the closing stage's section into the "what is done"
   form (status CLOSED, date, tag, key validation evidence).
2. Promote the next stage to ACTIVE.
3. Refresh the status table at the top.
4. Reconcile against `known-limitations.md` (resolved items
   removed there) and against `threat-model.md` (any
   reclassification recorded in section 8).

Editorial corrections (typos, dead links, formatting) do not
require this process and may be applied at any time.

## mkfs.beamfs userspace evolution

Discovered during the multi-FS head-to-head bench (2026-04-30):
mkfs.beamfs v1 only exposes two schemes via the `-s` option:

- `inline`           = scheme=2 UNIVERSAL_INLINE, single-block scope,
                       BEAMFS_DATA_INLINE_BYTES = 3824 user bytes max
- `inode-universal`  = scheme=2 variant (same single-block scope)

This is the **single-block scope limitation** documented in
`file_inline.c` lines 658-664 (`-EFBIG` returned by
`beamfs_inline_write_begin` if `pos + len > 3824`). It prevents fair
comparison with mainstream FS (ext4, btrfs, squashfs) on files larger
than 3824 bytes.

### Required scheme evolution for v3+

| Scheme name                 | Status | Use case                       |
|-----------------------------|--------|--------------------------------|
| `inline` (=2)               | v2 ✓   | Small data, RS-protected metadata |
| `inode-universal` (=2)      | v2 ✓   | Variant of inline              |
| `extent-multiblock` (planned) | v3   | Files > 3824 bytes (HPC, datasets) |
| `extent-multiblock-rs` (planned) | v3 | Multi-block + RS FEC on data    |
| `cow-snapshots` (planned)   | v4     | Snapshot-aware (compete with btrfs) |
| `journal-replay` (planned)  | v4     | Crash-consistency journal       |

Reference design exists in `context/INLINE-MULTIBLOCK-DESIGN.md`
(1004 lines, 11-15h focused work estimate).

## Multi-FS head-to-head bench requirements

For beamfs scientific validation against mainstream filesystems
(ext4, ext3, btrfs, squashfs) under RadFI live fault injection, the
following user-visible features are required:

1. mkfs.beamfs `-s extent-multiblock` for files > 3824 bytes
2. mkfs.beamfs `--block-size=N` (currently fixed 4KB)
3. mkfs.beamfs `-J / --journal-size` (planned for v4)
4. beamfsd live mount/umount support (currently mount-only)
5. `tune.beamfs` userspace for runtime FS introspection
   (parity coverage, RS event journal, dirty block bitmap)

Until these are available, the multi-FS bench is constrained to
**files ≤ 3824 bytes** to maintain comparison fairness across all
filesystems. This constraint is documented in test reports as a
**v2 INLINE scope boundary**, not as a limitation of the bench
methodology.

### Bench injector capability prerequisite

In addition to the mkfs.beamfs userspace evolution above, the
multi-FS head-to-head bench requires a **multifs-capable fault
injector**. The current emufi v0.3.5 implements file-precise
targeting via `target_block_range` calibrated per-FS, which
makes simultaneous N-FS comparison structurally impossible
(only one FS can be the calibration target per run). The
N=100 experiment of 2026-05-10 documented this limitation
empirically.

The required injector capability is tracked in
`~/git/emufi/Documentation/roadmap.md` as **emufi v0.4 -
Multi-FS capable injector** (estimated 95-115h focused work).
The emufi v0.4 release is therefore a precondition for the
publication-grade multifs bench described above.

## Other planned FS evolutions

- ZFS comparison: requires meta-zfs Yocto layer (4-8h yocto work)
- ext4 + dm-verity: read-only baseline with cryptographic integrity
- ZFS-on-Linux DKMS: out-of-tree comparison point (CDDL/GPL caveat)


---

## Mainline preparation roadmap

The path from current state to RFC mainline submission is broken
into 8 phases. Each phase has a clear Definition of Done (DoD), a
concrete deliverable, and an effort estimate in focused work hours.

### Phase 0 - Cadrage formalised

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 3 hours                                             |
| DoD         | Documents committed on `mainline-prep`              |
| Deliverable | mainline-scope.md, format-v5.md, fsck.beamfs.md |
| Risk        | Scope creep, mitigated by atomic commits            |
| Precondition| None                                                |

### Phase 1 - Format v5.0 minimal (RFC-able baseline)

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 50 hours                                            |
| DoD         | beamfs.h v5 + mkfs.beamfs userspace, format mountable R/W on device |
| Deliverable | Updated beamfs.h, tools/mkfs.beamfs/, format-v5.md complete |
| Risk        | Format break (acceptable, no production deployment) |
| Precondition| Phase 0                                             |

### Phase 2 - fsck.beamfs MVP

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 30 hours                                            |
| DoD         | fsck.beamfs --check-only passes 1-3, exit codes ok  |
| Deliverable | tools/fsck.beamfs/, fsck.beamfs.8 manpage, Test D   |
| Risk        | Low (scope well defined)                            |
| Precondition| Phase 1                                             |

### Phase 3 - Multiblock read_folio (VFS impedance resolution)

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 12 hours                                            |
| DoD         | 15 boundary tests pass, fsstress 1h no corruption   |
| Deliverable | file_inline.c sub-steps 4-10 complete, regression tests |
| Risk        | Race conditions, lock discipline                    |
| Precondition| Phase 1                                             |

### Phase 4 - Stage 4 close + paper v3 draft

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 100 hours (paper-heavy)                             |
| DoD         | Paper v3 draft + Zenodo upload + arXiv preprint     |
| Deliverable | papers/2026-XX-beamfs-v5/                           |
| Risk        | Paper drift                                         |
| Precondition| Phases 1, 2, 3                                      |

### Phase 5 - DKMS packaging + Yocto layer

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 25 hours                                            |
| DoD         | dkms install works on Debian/Ubuntu/Fedora/Gentoo, Yocto recipe |
| Deliverable | packaging/dkms.conf, packaging/{debian,rpm}/, meta-beamfs/ |
| Risk        | Distro variations                                   |
| Precondition| Phase 1                                             |

### Phase 6 - Build user base (anti-NAK FTRFS-style)

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 75 hours                                            |
| DoD         | 3-5 public deployments cited, blog post, linux-fsdevel presence |
| Deliverable | Site, technical blog, FOSDEM/equivalent submission, deployments referenced |
| Risk        | Time sink, must be boxed                            |
| Precondition| Phase 5                                             |

### Phase 7 - Documentation/filesystems mainline + checkpatch zero

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 50 hours                                            |
| DoD         | beamfs.rst Sphinx-ready, MAINTAINERS entry, checkpatch --strict zero, fs/beamfs/ restructure, selftests, fsstress + xfstests subset PASS |
| Deliverable | Patch series 15-25 commits ready for submission     |
| Risk        | Moderate                                            |
| Precondition| Phases 1-3 + Phase 6 produces 3+ deployments cited  |

### Phase 8 - RFC mainline + review cycle

| Item        | Detail                                              |
|-------------|-----------------------------------------------------|
| Effort      | 60 hours (variable)                                 |
| DoD         | Patch series merged OR explicit NAK with corrective actions for v5.1 |
| Deliverable | Cover letter linux-fsdevel, patch series, review responses |
| Risk        | NAK political (bcachefs-style ejection if code style fails) |
| Precondition| All previous phases                                 |

### Total

| Phase | Hours | Cumulative |
|-------|-------|------------|
| 0     | 3     | 3          |
| 1     | 50    | 53         |
| 2     | 30    | 83         |
| 3     | 12    | 95         |
| 4     | 100   | 195        |
| 5     | 25    | 220        |
| 6     | 75    | 295        |
| 7     | 50    | 345        |
| 8     | 60    | 405        |

**Total: 405 hours focused work** before mainline RFC submitted.

### Commit convention

Phase tracking via commit message footer:

feat(format-v5): add EXTENTS feature flag

phase=1
DoD: extents flag defined in beamfs.h, no behavior yet

[message body]

Signed-off-by: Aurélien Desbrières aurelien@roaster.lab


Filter via `git log --grep="phase=N"` to track per-phase progress.

### Precision tooling

To prevent drift across the 8 phases:

- This roadmap is the source of truth for phase status.
- Each phase has explicit DoD that must pass before moving to the next.
- `bin/phase-status.sh` (TBD Phase 0) reports % completion per phase.
- Weekly 30-minute "phase review" checkpoint internal to the project.
- Beamfs-bench acquired in earlier work serves as anti-regression
  harness for all subsequent phases.
