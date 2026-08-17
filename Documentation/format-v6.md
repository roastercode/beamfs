# beamfs On-Disk Format Specification (v6 delta): per-block data checksum

**Status**: Normative design baseline for the `DATA_CSUM` feature.
Companion to `format-v5.md`, `data-protection-design.md`,
`threat-model.md` section 6, and `known-limitations.md` section 3.11.
**Last updated**: 2026-07-09.
**Branch**: `devel`.
**Tag (planned at closure)**: `v0.6.0-data-csum`.

This document specifies a single on-disk change layered on the v5
format: an optional per-data-block integrity field, gated by a
RO-compat feature flag. It does not redefine the v5 format. Every
statement in `format-v5.md` remains in force; this document adds
one field in previously-reserved space and one feature bit.

---

## 1. What this closes, and what it does not

### 1.1 The failure mode

`known-limitations.md` section 3.11 records a silent Reed-Solomon
miscorrection on a data block under high-density electromagnetic
injection. The mechanism is intrinsic to any RS-only FEC layout:
when a subblock receives a symbol-error pattern beyond the
correctable bound `t = (n-k)/2`, `decode_rs8` may converge to a
different valid codeword than the one originally encoded, return
`nerr >= 0` (reported success), and hand back plausibly-shaped but
incorrect bytes with no kernel-visible signal.

The inode and superblock paths are immune to this class because,
after a successful RS decode, they recompute a CRC32 over the
decoded payload and reject the decode on mismatch (`i_crc32`,
`s_crc32`). The v5 data-block path has no such second source of
truth: format v5 reserves no integrity field on data blocks
independent of the RS codeword.

### 1.2 Scope of the fix

This document adds that second source of truth to data blocks. The
guarantee obtained is **observability**, not additional correction
capacity:

- With `DATA_CSUM` active, a silent miscorrection on a data block
  is **detected** after RS decode and converted into a fail-closed
  outcome (read returns `-EIO`, an `UNCORRECTABLE`-flagged journal
  entry is emitted), instead of returning wrong bytes silently.
- `DATA_CSUM` does **not** increase the number of symbol errors
  that can be corrected. A burst that exceeds RS capacity still
  fails; the change is that it now fails **loudly and safely**
  rather than silently.

The two properties live on orthogonal axes, and this document is
scoped strictly to the first:

| Axis | Failure addressed | Mechanism | On-disk gate |
|------|-------------------|-----------|--------------|
| Observability (this document) | Silent RS miscorrection (3.11) | Integrity field independent of RS algebra | `RO_COMPAT DATA_CSUM` (bit 4) |
| Correction capacity | Burst exceeding RS(255,239) per subblock (Family B) | Out-of-band stride parity placement | `INCOMPAT SHADOW_PARITY` (bit 11) |

The two features compose: a future volume may set both, obtaining
loud-and-safe failure plus burst-tolerant recovery. Neither
depends on the other. `SHADOW_PARITY` remains reserved and
unimplemented per `data-protection-design.md` section 5; this
document does not touch it.

### 1.3 Relation to Theorem v2.2b

The paper (`papers/2026-04-beamfs-v2`, Theorem v2.2b) states
saturation observability on data regions as a theorem conditional
on an integrity-field hypothesis: every data region carries a
field `c` whose recomputation on the decoded payload after RS
decode is a soundness predicate independent of the Reed-Solomon
code algebra. The `DATA_CSUM` field specified here instantiates
that hypothesis for adversary Family A (stochastic miscorrection):
CRC32 over the decoded payload fails, for a codeword-valid but
originally-unencoded payload, with probability at least
`1 - 2^-32` per event, independent of the RS algebra.

CRC32 (crc32_le, the primitive shared with the inode and
superblock paths) is unkeyed and linear. It therefore instantiates the
hypothesis for Family A only. Family B (adversarial saturation, an
attacker steering the decoder toward a chosen codeword) can, in
principle, craft a payload whose linear checksum matches the
target; a keyed or second-preimage-resistant field is required to
extend observability to Family B. The `csum_type` field
(section 3.2) reserves that extension without a further format
break. The on-disk choice of a keyed field is deferred; it is not
specified here.

---

## 2. Version and feature policy

`DATA_CSUM` is delivered as a **RO-compat** feature, not an
incompat one, and not a magic or structural version break. The
name "format v6" denotes the v5 format with this feature bit
available; it is not a hard fork. The rationale is the standard
feature-flag philosophy already adopted in `format-v5.md`
section 2 and section 4.3.

Compatibility matrix for a volume with `DATA_CSUM` set:

| Kernel | Behaviour |
|--------|-----------|
| `DATA_CSUM`-aware, feature set | Full read/write; csum computed on write, verified on read |
| Not `DATA_CSUM`-aware | Mounts **read-only** (unknown RO-compat bit forces RO per `format-v5.md` section 4.3.4 rule 2) |

The RO-compat class is the correct and minimal choice. An
unaware kernel that only reads is safe: it ignores the pad region
where the csum lives and decodes data exactly as in v5. The only
unsafe operation for an unaware kernel is **writing**: it would
rewrite the block, zero the pad, and leave a stale or absent csum
that an aware kernel would later read as a mismatch. Forcing RO on
unaware kernels removes exactly that operation and nothing more.
An incompat bit would be too strong (it would deny unaware kernels
even read access, which is safe), which is why bit 11
`SHADOW_PARITY` is incompat (it changes parity placement, so
unaware read is unsafe) while `DATA_CSUM` is ro-compat.

### 2.1 Feature bit allocation

Bit 4 of `s_feat_ro_compat` is the next free RO-compat position
(bits 0-3 are allocated in `format-v5.md` section 4.3.2). It is
allocated to `DATA_CSUM` by this document:

```c
#define BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM        (1ULL << 4)
```

RO-compat table, extended:

| Bit | Flag                                   | Meaning                                             |
|-----|----------------------------------------|-----------------------------------------------------|
| 0   | `BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE`  | Files exceed 4 GiB (i_size_high used)               |
| 1   | `BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE`   | Files exceed 16 TiB                                 |
| 2   | `BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE` | Inode size exceeds 256 bytes                        |
| 3   | `BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR`   | Directory entries stored in a B-tree                |
| 4   | `BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM`   | Per-data-block integrity field in the block tail pad |

When the activating patch series merges,
`BEAMFS_FEAT_RO_COMPAT_SUPP` (currently `0ULL`) gains this bit.
Until then, an image with bit 4 set forces RO mount on every
current kernel, which is the intended graceful degradation.

---

## 3. On-disk layout

### 3.1 Placement in the block tail pad

Under `s_data_protection_scheme = UNIVERSAL_INLINE` (2), a data
block is laid out as (see `beamfs.h` and `format-v4.md` section 7):

```
offset      size    content
0..4079     4080    16 interleaved RS(255,239) shortened subblocks
                    (3824 user payload bytes + 256 parity bytes)
4080..4095    16    tail pad, zero-filled at encode time
```

The tail pad `[4080, 4096)` is not part of any RS codeword and is
zero-initialised on write in v5 (`file_inline.c`, the two
`memset` sites at the block tail). `DATA_CSUM` repurposes the
first 8 bytes of this pad as a typed integrity descriptor. The
remaining 8 bytes `[4088, 4096)` stay zero-filled and reserved.

```
offset      size    field
4080          1     csum_type   (u8, see 3.2)
4081..4083    3     reserved    (MUST be zero on write)
4084..4087    4     csum        (__le32, value per csum_type)
4088..4095    8     reserved    (MUST be zero on write)
```

No user payload byte is consumed. Data capacity stays at 3824
bytes per block. Block and page alignment are unchanged. This is
the decisive difference from the M2 sketch in
`known-limitations.md` section 3.11, which proposed relocating the
csum into the payload and shrinking user bytes from 3824 to 3820:
that relocation is unnecessary because the pad already exists and
is already RS-excluded.

### 3.2 csum_type enumeration

```c
#define BEAMFS_CSUM_NONE     0   /* no integrity field present            */
#define BEAMFS_CSUM_CRC32    1   /* CRC32 (crc32_le) over decoded payload  */
/* values 2..255 reserved for keyed / second-preimage-resistant fields
 * (Family B observability); not specified by format v6                    */
```

A block whose `csum_type` is `BEAMFS_CSUM_NONE` on a `DATA_CSUM`
volume is treated as not-yet-checksummed and passes verification
unconditionally (see section 4.3). This admits lazy upgrade and
partially-written volumes without a hard error.

### 3.3 Integrity field: the csum is not RS-protected

The 8-byte descriptor lives outside the RS codeword, so it is not
error-corrected. A bit flip in the `csum` value field can only
produce a **false mismatch**: an aware kernel rejects a decode that
was in fact correct, returning `-EIO` on good data. That is an
availability cost, never a correctness cost, because a wrong
payload plus any csum value other than the wrong payload's own
CRC32 fails the check.

That argument covers the value field only. Earlier revisions of
this section generalised it to the whole descriptor, which was
wrong: `csum_type` gates whether the check runs at all, so a flip
turning it into any value other than `BEAMFS_CSUM_CRC32` was read
as "no checksum present" and the block was accepted unverified.
Measured 2026-08-17 under a 128-flip budget on a 64-block file: 20
RS symbols corrected, no uncorrectable event, no mismatch logged,
and 15296 wrong bits returned to userspace across two blocks.

The read path therefore fails closed on any `csum_type` other than
`BEAMFS_CSUM_CRC32` when the volume has `DATA_CSUM` set. Every data
block on such a volume is stamped at write time, so an unexpected
type is a corrupted descriptor rather than an unstamped block. A
flip in the type byte now costs availability, like a flip in the
value, and the design goal (no silent wrong bytes) holds for the
descriptor as a whole.

---

## 4. Semantics

### 4.1 Coverage

`csum` for `BEAMFS_CSUM_CRC32` is the CRC32 computed by
`beamfs_crc32` (the kernel `crc32_le` IEEE variant, XOR-inverted),
the exact same primitive already used for `i_crc32` and `s_crc32`.
It is computed over the **3824-byte decoded user payload** of the
block, in logical (de-interleaved) order, including any trailing
zero pad written to fill a partial final block. It is computed
over the payload as it exists after RS decode on read, and before
RS encode on write. It does not cover the parity bytes, the tail
pad, or the descriptor itself.

Covering the decoded payload (not the raw on-disk codeword) is
required, not merely conventional:

- It matches `i_crc32` and `s_crc32`, which both cover decoded
  content. The data path then presents a single, homogeneous
  integrity discipline across inode, superblock, and data regions,
  rather than a third distinct semantics for an auditor to review.
- It is the coverage Theorem v2.2a proves the predicate over, so
  the v2.2b proof transfers the v2.2a argument verbatim.
- It is the only coverage that detects the target failure. A
  silent miscorrection produces a **valid** on-disk codeword (a
  different one); a checksum over the raw codeword would match that
  valid codeword and miss the bug. Only a checksum over the
  expected decoded payload diverges when the decoder yields the
  wrong payload.

### 4.2 Write path

On writeback of a data block under a `DATA_CSUM` volume:

1. Assemble the 3824-byte user payload (zero-fill a partial final
   block to 3824 as in v5).
2. Compute `crc = beamfs_crc32(payload, 3824)`.
3. RS-encode the payload into the 16 interleaved subblocks
   (unchanged from v5).
4. Write `csum_type = BEAMFS_CSUM_CRC32`, `reserved = 0`,
   `csum = crc` into the tail-pad descriptor; zero the remaining
   pad bytes.

The csum is computed on the same payload bytes that feed the RS
encoder, so encode and checksum are consistent by construction.

### 4.3 Read path

On `read_folio` of a data block under a `DATA_CSUM` volume:

1. RS-decode the 16 subblocks into the 3824-byte payload buffer
   (unchanged from v5).
2. If `csum_type == BEAMFS_CSUM_NONE`, accept (lazy-upgrade
   tolerance, section 3.2).
3. If `csum_type == BEAMFS_CSUM_CRC32`, compute
   `beamfs_crc32(decoded_payload, 3824)` and compare with the stored
   `csum`. On match, accept. On mismatch, fail closed:
   - return `-EIO` for the folio;
   - emit an RS-journal entry via `beamfs_log_rs_event` with
     `positions = NULL`, `n_positions = 0`, and the
     `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE` flag, carrying the
     `(block, subblock)` coordinates, committed before the read
     returns. This is the same observability mechanism proved for
     `Lambda_crc` in Theorem v2.2a, now extended to `Lambda_data`.
4. If `csum_type` is an unknown non-zero value on a kernel that
   knows `DATA_CSUM` but not that subtype, fail closed as in step 3
   (conservative: an unknown integrity discipline is treated as
   unverifiable, not as absent).

This is the exact analogue of `inode.c::beamfs_iget`: RS decode,
then recompute the independent integrity field, then reject on
mismatch. The data path gains the second source of truth it
lacked.

---

## 5. mkfs.beamfs and migration

### 5.1 Format time

`mkfs.beamfs --profile=embedded --data-csum` (flag name subject to
section 7) sets `BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM` in
`s_feat_ro_compat` and, for every data block it writes (including
the canary fixture), computes and stores the CRC32 descriptor per
section 4.2. Volumes formatted without the flag are byte-identical
to v5 and set no new bit.

### 5.2 Migration

- **Copy migration** (always available):
  `mkfs.beamfs --profile=embedded --data-csum` on a fresh volume
  plus `rsync -aHAX`. Every block is written through the v6 write
  path and acquires a csum.
- **In-place upgrade** (deferred, sketch only): a userspace
  `tune.beamfs --add-data-csum` walks every data block, decodes,
  computes the csum, writes the descriptor into the tail pad, and
  sets the RO-compat bit in the superblock last. Because the bit is
  RO-compat, an interrupted upgrade leaves a volume that unaware
  kernels still mount read-only and aware kernels can resume;
  blocks not yet stamped read as `BEAMFS_CSUM_NONE` and pass
  (section 3.2), so the walk is restartable. Full specification is
  out of scope here.

---

## 6. Honest scope statement

To be reproduced (or paraphrased without weakening) in the RFC
cover letter, the paper v3 abstract, and `Kconfig` help text, in
the same spirit as `data-protection-design.md` section 7:

> The `DATA_CSUM` feature makes silent Reed-Solomon miscorrection
> on data blocks observable: under electromagnetic stress that
> defeats the RS code, a read fails closed with `-EIO` and an
> auditable journal entry rather than returning incorrect bytes.
> It detects the failure; it does not add correction capacity.
> Detection covers Family A (stochastic miscorrection) with
> per-event probability at least `1 - 2^-32`. Family B
> (adversarial saturation) can defeat the unkeyed CRC32; keyed
> integrity for Family B is reserved through the `csum_type` field
> and is not claimed by this feature. Burst-tolerant recovery for
> Family B remains the separate, reserved `SHADOW_PARITY` feature.

---

## 7. Open items deferred to session-level decision

Not resolved by this document; left for explicit arbitration
before code lands:

- **7.1 mkfs.beamfs flag name.** `--data-csum` names the
  mechanism; alternative `--detect-miscorrection` names the
  guarantee. TBD.
- **7.2 Whether the RO-compat bit is set by default in the
  embedded profile at v6 closure, or opt-in for one release** to
  gather field data before making it the default. Anti-NAK
  consideration: shipping it opt-in first lets the RFC cite it as
  a validated-but-optional hardening rather than a mandatory
  format change. TBD.
- **7.3 fsck.beamfs behaviour** on a `csum_type` mismatch found
  during offline scan (repair from parity vs report-only). TBD,
  tracked with the Phase 2 fsck deliverable.
