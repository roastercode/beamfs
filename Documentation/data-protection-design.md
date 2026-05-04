# beamfs Data Protection Design (Stage 4)

**Status**: Stage 4 design baseline. Companion to `roadmap.md`
Stage 4 and to `mainline-scope.md` section 4.
**Last updated**: 2026-05-04.
**Branch**: `mainline-prep`.
**Tag (planned at closure)**: `v0.4.0-universal-protection`.

---

## 1. Purpose

This document records the architectural decision on the data block
Reed-Solomon protection scheme for beamfs v5.0, the on-disk format
that is the subject of the first beamfs RFC submission to
linux-fsdevel.

Three candidate schemes were enumerated in `design.md` and reserved
in `beamfs.h` (lines 309-331):

- `BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE`  (value 2)
- `BEAMFS_DATA_PROTECTION_UNIVERSAL_SHADOW`  (value 3)
- `BEAMFS_DATA_PROTECTION_UNIVERSAL_EXTENT`  (value 4)

Stage 4 of `roadmap.md` requires a written comparison and a chosen
scheme before implementation work begins. This document is that
comparison. It also records the resolution path for the schemes
not selected for v5.0.

This document is normative for the v5.0 RFC. Subsequent format
revisions (v5.1+) may revisit the decision through the standard
feature-flag evolution path defined in `format-v5.md` section 4.

## 2. Constraints (sources of truth)

The decision is constrained by four independent axes, each backed
by a source of truth in this repository.

### 2.1 Threat model coverage

`threat-model.md` defines two adversary families:

- **Family A** (section 2.1): stochastic electromagnetic
  perturbations. Single bit flips arriving as a Poisson process,
  spatially uncorrelated. Per-block RS(255,239) is sufficient.
- **Family B** (section 2.2): adversarial electromagnetic events.
  Spatially and temporally correlated bursts that may exceed
  RS(255,239) capacity per sub-block. Per-block in-block RS is
  necessary but **not sufficient**.

`threat-model.md` section 6.2 falsifies in-block RS for Family B
bursts that exceed 256 octets, in this exact wording:

> A naive in-block sub-RS layout (16 sub-blocks of RS(255,239)
> packed inside one 4 KiB block) is sufficient for Family A but
> insufficient for Family B at the scale of bursts that exceed
> 256 bytes.

The viable architectures named in section 6.2 are out-of-band
shadow regions with deliberate stride placement, or extent-based
layouts where the parity attribute can specify a distribution
pattern. These are schemes 3 and 4 above.

### 2.2 Auditable code budget

`threat-model.md` section 6.5 fixes the auditable kernel-side code
budget at **5000 lines**, sourced from the project Kconfig and
motivated by the certification regimes named in section 7
(DO-178C, ECSS-E-ST-40C, IEC 61508, MIL-STD-882E). Filesystems
above 5000 LOC (ext4 ~100k, btrfs ~200k) are non-starters in those
regimes.

Current count, measured 2026-05-04 on the snapshot at HEAD
`d30ccd1`:

```
   444 alloc.c
   199 dir.c
   417 edac.c
   261 file.c
  1249 file_inline.c
   201 inode.c
   629 namei.c
   798 super.c
   682 beamfs.h
  4880 total
```

**Remaining margin: 120 LOC** (5000 - 4880). The figure of 2280
LOC quoted in `threat-model.md` section 6.5 is from an earlier
measurement and is updated by this document.

### 2.3 Anti-NAK doctrine

`context-recadrage.md` "Acquis stratégiques Phase 0" fixes the
RFC trajectory:

> RFC initial = profile `embedded` minimal seulement (~5-8k LoC,
> anti-NAK fort). Patches successifs ajoutent les flags un par un.
> C'est la trajectoire f2fs / exfat (entrée modeste, croissance
> par patches), opposée a bcachefs (entrée massive, éjection
> 2025).

Two reviewer constraints from `roadmap.md` "RFC v4 readiness"
section additionally apply:

- Pedro Falcato flagged the opt-in model (scheme 1
  INODE_OPT_IN, deprecated) as a use-case scope problem. The
  replacement architecture must be universal (TM 6.1).
- `roadmap.md` Phase 8 risk row names the failure mode
  explicitly: "NAK political (bcachefs-style ejection if code
  style fails)".

The doctrine excludes any RFC v0 submission whose scope exceeds
the smallest mountable v5.0 baseline.

### 2.4 Long-term architectural objective

`roadmap.md` section "Principal long-term objective: Full beamfs
rootfs" registers the canonical long-term direction:

> The principal long-term engineering objective of the beamfs
> project is to be deployable as a full Linux rootfs, replacing
> squashfs on production OIV-grade systems where electromagnetic
> resilience is a stated design constraint.

Subsequent design decisions (this document included) are
evaluated against that objective. The objective requires
arbitrary file sizes (rootfs binaries, libraries, configs), full
POSIX semantics, xattr / SELinux, and verified-boot integration.
None of these depend on the data block scheme choice in
isolation, but the scheme must not foreclose them.

## 3. Comparison

The three candidate schemes are evaluated against the four axes
defined in section 2.

### 3.1 Coverage matrix

| Axis | Scheme 2 INLINE | Scheme 3 SHADOW | Scheme 4 EXTENT |
|---|---|---|---|
| TM 6.1 universal coverage | yes | yes | yes |
| TM 6.2 burst tolerance Family B | **no** | yes | yes |
| TM 6.5 LOC budget under 5000 | yes (~50-100 LOC remaining) | no (~600-1000 LOC delta) | no (~1200-1800 LOC delta) |
| Certification tractability (DO-178C, ECSS-E-ST-40C, IEC 61508, MIL-STD-882E) | preserved (under 5000 LOC) | requires Kconfig amendment | out of audit envelope |
| Anti-NAK doctrine (embedded minimal RFC) | conformant | hostile (scope creep at v0) | hostile (extent tree at v0) |
| Long-term full-rootfs objective | enables (multi-block path closed, EXTENTS flag reserved) | enables | enables |

### 3.2 Implementation footprint

| Item | Scheme 2 | Scheme 3 | Scheme 4 |
|---|---|---|---|
| read_folio path | implemented (file_inline.c) | new dual-bio (data + parity) | new extent-attr lookup |
| writeback path | implemented (file_inline.c) | new dual-bio + crash ordering | new extent attr update |
| mkfs.beamfs | `--profile=embedded` already emits scheme=2 | new shadow region allocator | new extent allocator |
| fsck.beamfs (Phase 2) | walk SB + bitmap + inodes + RS journal | + walk shadow region + pair data/parity | + walk extent tree + per-extent parity |
| Crash atomicity | natural (single block write) | requires journal ordering | requires journal ordering |
| Theorem v2.1 (Zenodo paper v2) | preserved | requires new Theorem v3 on stride distribution | requires new Theorem v3 on extent distribution |
| Empirical footprint already acquired | RECOVERED 12/12 cluster + multifs RECOVERED 3/3 + neutralisation asymmetry findings 2026-04-30 + canary fixture (format-v5.md section 8) | none | none |

### 3.3 RFC submission impact

| Item | Scheme 2 | Scheme 3 | Scheme 4 |
|---|---|---|---|
| Scope at submission | `embedded` profile, 0 feature flag active, scheme=2 baseline | scheme=3 in v0 = scope creep | scheme=4 in v0 = scope creep |
| Reviewer pattern (Falcato, Wilcox, Wong, Dilger, Biggers) | folio/iomap discipline already validated, RS API correctness already validated (Stage 3 Item 3) | re-validation of all reviewer-touched paths | re-validation + extent abstraction defence |
| Anti-FTRFS-NAK Phase 6 user base | 3-5 deployments achievable on stable scheme=2 (DKMS + Yocto layer) | blocked until implementation closes | blocked until implementation closes |
| Calendar to RFC mail | 3-4 months sustainable | 5-7 months | 8-12 months |

## 4. Decision

beamfs v5.0 RFC submits with `s_data_protection_scheme = 2`
(`UNIVERSAL_INLINE`).

This is the only choice that satisfies all four constraint axes
of section 2 simultaneously. Schemes 3 and 4 each violate at
least one axis (LOC budget, certification envelope, anti-NAK
doctrine), and each forces a new soundness theorem and a new
empirical campaign that the calendar of `roadmap.md` Phase 4
does not absorb.

The decision is **conditional**: it holds for the v5.0 RFC
only. Family B coverage (TM section 6.2) is explicitly **not
claimed** by v5.0. The path to Family B coverage is
documented in section 5 below and is reserved on-disk through
a feature flag bit.

## 5. Reservation for scheme 3 (post-merge evolution)

beamfs v5.0 reserves the on-disk feature flag bit
`BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY` for the future
implementation of scheme 3 `UNIVERSAL_SHADOW`.

### 5.1 Bit allocation

INCOMPAT bits 0-10 are allocated in `beamfs.h` and
`format-v5.md` section 4. Bit 11 is the next available position
and is allocated to SHADOW_PARITY by this document:

```
#define BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY  (1ULL << 11)
```

This bit is **declared** in `beamfs.h` and **not** added to
`BEAMFS_FEAT_INCOMPAT_SUPP`. A v5.0 mount that encounters this
bit on an image refuses to mount per the standard INCOMPAT
semantics defined in `format-v5.md` section 4.

### 5.2 Semantics (when activated, post-v5.0)

When `BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY` is set, the volume
stores RS parity in a dedicated out-of-band region with stride
placement chosen so that a localised burst affecting N adjacent
physical sectors does not exceed the correction capacity of any
single codeword. The on-disk layout details, the stride policy,
and the read/write path are defined in a future
`Documentation/format-v5.1.md` revision.

### 5.3 Coexistence with scheme 2

Volumes formatted with scheme=2 today remain mountable on a
SHADOW_PARITY-aware kernel. The `s_data_protection_scheme` field
selects scheme 2; the SHADOW_PARITY incompat bit is not set on
those volumes; the read/write path uses the scheme=2 in-block
layout described in `format-v5.md` section 7.

A volume formatted with `--profile=embedded --shadow-parity`
(future mkfs.beamfs flag, post-v5.0) sets
`BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY` and uses
`s_data_protection_scheme = 3`.

### 5.4 Migration

Migration from scheme=2 to scheme=3 is out of scope for v5.0 and
is sketched only for completeness:

- Copy migration: always available (`mkfs.beamfs --profile=...
  --shadow-parity` plus `rsync -aHAX`).
- In-place upgrade: requires a userspace `tune.beamfs --upgrade
  scheme=3` tool that allocates the shadow region, copies parity
  out-of-band, and clears the in-block parity zone. Deferred to
  v5.x or beyond.

## 6. Reservation for scheme 4 (deferred)

Scheme 4 `UNIVERSAL_EXTENT` is **not** allocated a feature flag
bit by this document. It remains reserved as an enum value in
`beamfs.h` (line 331) and may be promoted to a feature flag if
post-merge experience indicates it is needed.

The rationale is that scheme 3 with appropriate stride is
expected to cover Family B at the scale of bursts up to a few
KiB without the complexity of an extent-based parity layout.
Scheme 4 is retained as a future option only.

## 7. Honest scope statement (cover letter, paper, Kconfig)

Every public-facing artefact of v5.0 must declare the Family A vs
Family B scope explicitly. The wording below is normative and is
to be reproduced (or paraphrased without weakening) in:

- the RFC cover letter (Phase 8, `upstream-5`)
- the paper v3 abstract (Phase 4, `upstream-7`)
- `Kconfig` help text for `CONFIG_BEAMFS_FS`
- `Documentation/threat-model.md` section 6.2 (amended to
  distinguish v5.0 baseline from v5.x burst-tolerant)
- `Documentation/mainline-scope.md` section 2 (amended to
  qualify the threat-model coverage claim)

Normative wording:

> beamfs v5.0 protects against Family A perturbations
> (stochastic electromagnetic single-bit and multi-bit upsets,
> aging silicon bit-rot, environmental radiation,
> rowhammer-class single-cell flips). Family B perturbations
> (adversarial bursts exceeding 256 octets per sub-block, IEMI
> at scale, voltage-glitch attacks at scale) are not claimed by
> v5.0. The on-disk format reserves the feature flag bit
> `BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY` for a future v5.x
> revision that adds out-of-band parity with stride placement,
> targeting Family B coverage. Coexistence between the two
> schemes is supported by design.

## 8. Exit conditions of Stage 4 (this document is part 1 of 3)

Stage 4 closure requires three deliverables:

1. **This document**, signed off and committed on
   `mainline-prep`. (Status of the present commit.)
2. **Implementation** of the Stage 4 sanity tests defined in
   `roadmap.md` Stage 4 "Exit conditions". For scheme=2, the
   already-acquired empirical footprint
   (`papers/2026-04-beamfs-v3-findings/SCIENTIFIC-FINDINGS-2026-04-30.md`)
   covers items 1-4 of the Stage 4 exit list. Item 5
   (throughput documented) requires a fresh measurement on the
   v5.0 mainline-prep tip and is part of the paper v3 empirical
   chapter.
3. **Bit 11 SHADOW_PARITY allocation** committed in `beamfs.h`
   and documented in `format-v5.md` section 4 INCOMPAT table.
   No code path uses the bit; the declaration is reservation
   only.

The xfstests subset (`generic/{001,002,010,098,257}`), the
clean atomic patch series, and the response to open reviewer
comments are also Stage 4 closure conditions per `roadmap.md`
"RFC v4 readiness" but are tracked as separate deliverables
(Phase 7 `upstream-1`, Phase 8 patch series, Phase 8
`upstream-5`).

## 9. Open items deferred to session-level decision

The following sub-decisions are **not** resolved by this document
and are intentionally left for explicit session arbitration
before any commit lands:

- **9.1 mkfs.beamfs flag name**: `--shadow-parity` is the
  candidate above; alternative is `--family-b` with the rationale
  that the user-visible knob names the threat coverage, not the
  implementation. TBD.
- **9.2 Threat-model.md section 6.2 amendment wording**: the
  amendment must distinguish v5.0 baseline (in-block, Family A)
  from v5.x burst-tolerant (out-of-band, Family A + Family B)
  without weakening the falsification statement against in-block
  RS for Family B at scale. Draft TBD.
- **9.3 Kconfig help text**: integrate the section 7 normative
  wording into the existing help block. Draft TBD.
- **9.4 Paper v3 abstract Family A/B framing**: the empirical
  findings 2026-04-30 already exhibit cluster-wide RECOVERED
  under saturation injection. The abstract must position this
  as a Family A claim at saturation (not as a Family B claim),
  to remain consistent with section 7. Draft TBD.

## 10. References

- `Documentation/threat-model.md` sections 2.1, 2.2, 6.1, 6.2,
  6.5, 7
- `Documentation/roadmap.md` Stage 4, "RFC v4 readiness",
  "Principal long-term objective: Full beamfs rootfs"
- `Documentation/mainline-scope.md` sections 2, 4, 6
- `Documentation/format-v5.md` sections 4, 7, 8, 11
- `Documentation/design.md` "Data protection schemes"
- `context/context-recadrage.md` "Acquis stratégiques Phase 0",
  section 0 "Mapping de versioning"
- `papers/2026-04-beamfs-v3-findings/SCIENTIFIC-FINDINGS-2026-04-30.md`
- `beamfs.h` lines 309-385 (scheme enum and feature flag bits)
