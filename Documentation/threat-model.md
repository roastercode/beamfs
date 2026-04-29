# BEAMFS Threat Model and Market Positioning

**Status**: design document, normative for BEAMFS architectural decisions.
**Audience**: kernel reviewers, certification auditors, downstream integrators,
operators of safety- or mission-critical embedded Linux systems.
**Last updated**: 2026-04-25.

---

## 1. Purpose

BEAMFS is the **B**eam **E**lectro**M**agnetic **F**ile **S**ystem. Its
thesis: data-at-rest integrity on a single read-write device under the
full spectrum of electromagnetic perturbations — stochastic and
adversarial, ionizing and impulsive, background and burst.

This document defines the failure modes BEAMFS is designed to mitigate,
the threat actors it considers in scope, and the existing Linux/BSD
storage stack capabilities it complements or extends. It is the
normative reference for design trade-offs in the on-disk format,
the IO path, and the roadmap. Subsequent design documents (`design.md`,
`roadmap.md`, `system-architecture.md`) refine this model; they do not
override it.

**Scientific lineage.** BEAMFS extends the FTRFS lineage (Fuchs, Langer,
Trinitis, *BEAMFS: A Fault-Tolerant Radiation-Robust Filesystem for
Space Use*, ARCS 2015) from a radiation-only threat model toward a
**unified electromagnetic resilience** threat model. The motivation is
empirical: the RadFI fault-injection tool (companion repository,
v0.1.0, 2026-04-28) demonstrated that filesystems designed strictly for
stochastic radiation upsets do not retain their soundness guarantees
under adversarial electromagnetic events that produce correlated
multi-byte bursts. This falsification motivates BEAMFS v2 and the
broadened taxonomy below.

A separate companion document — to be added — will define the
verification and validation strategy aligned with this threat model
(fault injection scenarios, acceptance thresholds, traceability matrix
for DO-178C, ECSS-E-ST-40C, IEC 61508).

---

## 2. Failure model

BEAMFS treats data-at-rest corruption on a single read-write device
as a single physical phenomenon — **electromagnetic perturbation of
stored charge or magnetic domains** — that manifests through two
statistically distinct families. Both families are electromagnetic in
origin; they differ in causal source, statistical signature, and
operational consequence. A third sub-section describes the **Reed-
Solomon saturation boundary**, an attribute that may be reached by
either family and that BEAMFS records explicitly.

### 2.1 Family A — Stochastic electromagnetic perturbations

**Origin.** Background single-event upsets from cosmic rays,
atmospheric neutrons, alpha particles from package contamination;
MRAM cell aging; NOR/NAND flash retention loss; thermal stress in
industrial environments; low-level RF environmental noise. All of
these are electromagnetic in nature — ionizing radiation is
high-energy EM, thermal stress is broadband EM, RF noise is
narrowband EM — and produce indistinguishable signatures at the
storage cell. Effects are well documented in the literature on
radiation-tolerant computing, flash endurance, and EMC.

**Statistical signature.**

- Spatial: errors are uniformly random across the device surface.
  No correlation between adjacent blocks.
- Temporal: errors occur as a Poisson process with a low constant
  rate (per-bit error rate dominated by altitude, latitude, technology
  node, ambient temperature, and ambient EM field strength).
- Magnitude per event: typically 1 to 8 bit flips per affected
  256-byte sub-block, well within the correction capacity of
  RS(255,239).

**Operational implication.** Detection without correction (the
ext4/btrfs/dm-verity-without-FEC posture) is sufficient to alert,
but insufficient to keep the system running through long unattended
missions. In-place correction is required. This is the regime that
FTRFS (Fuchs et al., 2015) was designed to address, and BEAMFS
preserves all of FTRFS's guarantees within this regime.

### 2.2 Family B — Adversarial electromagnetic events

**Origin.** Intentional electromagnetic interference (IEMI), high-power
microwave (HPM) directed-energy weapons, electromagnetic pulse (EMP)
from nuclear or non-nuclear sources, hostile EM jamming and pulsing in
conflict zones, deliberate exposure of captured equipment to RF
attack chambers, conducted EMI from compromised power infrastructure.

This family is no longer hypothetical. The convergence of small
unmanned platforms with EM weapons has been analyzed publicly as a
strategic concern (SemiVision Research, *Drones and EMP Weapons:
The Convergence and Strategic Implications for Future Battlefields*,
February 2026). EMP-resilient data storage is the subject of active
academic research (Far, Qazani, Rad, *Emp-secure data storage through
biohybrid and neuromorphic paradigms*, Discover Applied Sciences,
2026). The category is operationally distinct from Family A: it
violates the Poisson assumption that radiation-only filesystems
(including FTRFS) rely on for their soundness theorems.

**Statistical signature, in contrast with Family A.**

- Spatial: errors may be **correlated and localized**. A directed
  EM event may corrupt a contiguous range of physical blocks, an
  entire die, or all blocks accessed during a brief power transient.
- Temporal: errors arrive in **bursts** rather than as a steady
  Poisson process. A single event may inject the equivalent of
  months of background stochastic upsets in milliseconds.
- Magnitude per event: may **exceed RS(255,239) correction capacity
  per sub-block** (more than 8 symbol errors per 256 bytes). Naive
  per-block FEC is insufficient.
- Targeting: an attacker may **selectively** target metadata
  structures (superblock, inode table, allocation bitmap) to
  amplify damage with minimal energy expenditure.

**Operational implication.** Per-block RS(255,239) is necessary but
not sufficient for Family B. The architecture must additionally
provide:

1. **Burst tolerance** through interleaving or cross-block parity
   distribution, so that a localized event affecting N contiguous
   symbols is spread across N independent codewords each receiving
   one symbol error.
2. **Universal coverage**, not opt-in. An adversary will not
   restrict attacks to files marked with an "RS-enabled" attribute.
   All data blocks in a mounted BEAMFS volume must be protected by
   default.
3. **No security-relevant flag readable from the storage device
   alone.** A protection scheme whose activation depends on a flag
   in the on-disk inode is only as robust as the inode itself; if
   the inode is corrupted by the same event, the protection
   evaporates.
4. **Tamper-evident event journal.** The Electromagnetic Resilience
   Journal must record corrections in a way that allows a post-event
   forensic distinction between Family A and Family B regimes.

### 2.3 Reed-Solomon saturation boundary

Both Family A and Family B can produce events whose magnitude **exceeds
the correction capacity** of a single RS(255,239) shortened codeword
(more than 8 symbol errors within a single 256-byte subblock). At the
saturation boundary, the codeword is uncorrectable and the affected
data is irretrievable from the on-disk image alone.

Saturation is **not a third family**: it is an **attribute** that may
be reached from either family. A burst from Family B is the most
common cause, but a coincident cluster of Family A events within a
single subblock during a long-duration deployment can also reach the
boundary. Discrimination of cause is performed post-process by the
operator or forensic analyst, using temporal and spatial clustering
of adjacent journal entries; it is not in-kernel.

**Empirical motivation.** The RadFI fault-injection tool (companion
repository v0.1.0, 2026-04-28) was designed to drive a BEAMFS v1 image
into the saturation regime under controlled, reproducible parameters.
It empirically falsified the v1 soundness theorem on data blocks under
single-bit flip injection at submit_bio_noacct, motivating the v2 INLINE
scheme described in `Documentation/format-v4.md`. The saturation
boundary is therefore not a theoretical concern: it is the
experimentally validated frontier between recoverable and unrecoverable
data, and it must be observable to operators.

**Operational implication.** The on-disk event journal must record
uncorrectable events alongside corrected events, with sufficient
metadata (block number, timestamp) for clustering analysis. The
filesystem must propagate `-EIO` to userspace on uncorrectable read
(no silent data loss). See `Documentation/format-v4.md` section 6.5
for the on-disk encoding (`BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE`).

### 2.4 What BEAMFS explicitly does not address

This threat model is restricted to integrity of data at rest. The
following are out of scope and addressed by other layers of a
hardened system:

- **Confidentiality.** Disk encryption (dm-crypt, LUKS) is
  orthogonal and recommended below BEAMFS where required.
- **Authenticated metadata against an active write-capable
  adversary.** A future extension may add per-block hash-based
  signatures (NIST FIPS 205 SLH-DSA / SPHINCS+); this is mentioned
  in the long-term roadmap and is not part of v1.
- **Fault tolerance against device-level catastrophic failure**
  (chip burn-out, controller failure). BEAMFS operates within a
  single device. Multi-device redundancy is the role of the block
  layer (mdraid, dm-integrity over multiple devices, ZFS, btrfs
  multi-device).
- **Real-time bounded error recovery latency.** A future kthread
  scrubber with RT priority is on the roadmap; the current
  implementation corrects on read access.

---

## 3. Threat actor profile

For Family B, the relevant adversary profile is:

- **Capability**: can induce localized or wide-area EM events against
  a deployed platform. Cannot necessarily achieve persistent write
  access to the storage medium through normal IO interfaces.
- **Motivation**: degrade or destroy data on captured, lost, or
  exposed assets; force fail-stop on critical infrastructure;
  contaminate evidence on forensic targets.
- **Knowledge**: assumed to know the BEAMFS on-disk format, the
  RS parameters, and the mount-time recovery procedure. Security
  through obscurity is rejected.

For Family A, there is no actor; the model is purely physical. BEAMFS
must remain operationally correct under both, simultaneously, without
mode switching.

---

## 4. Reference deployment scenarios

The following deployments are within scope and have been used to
validate the requirements above. They are not theoretical: each maps
to an existing class of system observed in the field as of 2026.

### 4.1 Long-duration spaceborne Linux on COTS SoCs

Constellations such as Starlink, OneWeb, and Project Kuiper have
filed with the U.S. FCC plans deploying thousands of satellites in
LEO using COTS processors running Linux. Soft-error rates on 14 nm
and 40 nm SoCs under proton irradiation have been measured publicly
(e.g., arXiv 2503.03722, *When Radiation Meets Linux*) and constitute
the canonical empirical baseline for Family A in space deployments.
Existing reliability mitigations on these platforms are limited to
ECC memory; filesystem-level correction is absent from the stack.

This is Family A territory at scale, and historically the regime FTRFS
was designed for. BEAMFS provides the missing filesystem layer with
a strict superset of FTRFS's guarantees.

### 4.2 Military and dual-use unmanned platforms

Combat and surveillance drones, autonomous ground vehicles, and
loitering munitions increasingly operate in EM-contested airspace.
Engagement scenarios documented in 2026 include directed RF
neutralization, GPS spoofing, and deliberate HPM exposure. The
storage subsystem of a captured or downed asset is a target both
for forensic recovery by the captor and for forensic denial by the
operator.

This is Family B territory. BEAMFS protects the integrity of the
data partition through the EM event; complementary mechanisms (remote
or autonomous secure erase) handle the destruction-on-capture case.

### 4.3 Industrial and nuclear operating environments

Robots and instrumentation deployed inside reactor containments,
spent-fuel handling cells, particle accelerator tunnels, and certain
medical imaging facilities operate under continuous neutron and gamma
flux — high-energy electromagnetic and particle environments where
Family A rates are several orders of magnitude above background.
Mission durations span months to years; in-situ maintenance is
impossible or extremely costly.

This is sustained Family A territory with elevated rates and a
non-negligible probability of reaching the saturation boundary (sec
2.3) over mission lifetime. The operational requirement: in-place
correction, persistent event journal for predictive maintenance, and
explicit recording of saturation events for end-of-life planning.

### 4.4 Critical infrastructure exposed to grid-scale EMP

Substations, telecom hubs, water treatment SCADA, hospital
infrastructure. The threat surface here mixes Family A (background)
and Family B (intentional or solar-storm EMP). Protecting the data
partition of a hardened embedded controller is a distinct problem
from protecting the rest of the building.

### 4.5 Edge AI and physical-AI platforms

Embedded World 2026 documented production rollout of physical-AI
robotics, humanoid platforms, and large-fleet edge AI deployments
(Arm, MediaTek Genio Pro, ST, NXP, QNX). These platforms run Linux
with on-device ML inference, accumulate state on local storage,
and operate untethered for extended periods. They are increasingly
deployed in environments where Family A and occasionally Family B
apply (industrial sites near high-power equipment, urban
environments with dense RF, military and dual-use programs).

---

## 5. Existing Linux and BSD storage capabilities — gap analysis

The following table maps BEAMFS's threat model against the protections
provided by mainline Linux and BSD storage components as of April 2026.

| Mechanism                | Detection | Correction | Single device | RW partition | In kernel | Auditable LOC |
|--------------------------|-----------|------------|---------------|--------------|-----------|---------------|
| ext4 metadata checksums  | yes       | no         | yes           | yes          | yes       | ~100k         |
| btrfs / ZFS checksums    | yes       | only with redundancy (mirror, RAID, copies>1) | no | yes | yes | ~200k / ~370k |
| dm-verity (no FEC)       | yes       | no         | yes           | **no, RO only** | yes  | small |
| dm-verity + FEC          | yes       | yes (Reed-Solomon) | yes (parity on same or other device) | **no, RO only** | yes | small |
| dm-integrity standalone  | yes       | no         | yes           | yes          | yes       | small |
| dm-integrity + mdraid    | yes       | yes        | **no, requires multiple devices** | yes | yes | medium |
| EDAC kernel framework    | reporting only — for memory and caches, not block storage | n/a | n/a | n/a | yes | medium |
| rsbep / blkar / SeqBox   | yes       | yes        | yes           | yes via FUSE | **no, userspace** | n/a |
| HAMMER2 (DragonFly BSD)  | yes       | only with dedup or copies>1 | partial | yes | yes | ~50k |
| UFS2, FFS2 (BSD)         | minimal   | no         | yes           | yes          | yes       | medium |

**Reading of the table.**

The single combination of capabilities required by the BEAMFS threat
model — **detection AND correction AND single device AND read-write
AND in-kernel** — is not provided by any existing component. The
closest match, dm-verity + FEC, fails on the read-write requirement
(documented restriction: `target is read-only`). The next closest,
dm-integrity + mdraid, fails on the single-device requirement (it
requires multiple devices to gain correction capability).

This is not an accidental gap. It reflects an architectural
assumption embedded in the Linux storage stack: that data integrity
on a single read-write device is the application's problem, not the
filesystem's. That assumption is reasonable for general-purpose
servers and desktops where multi-device redundancy is cheap and
where applications can tolerate fail-stop on uncorrectable errors.

It is not reasonable for the deployment scenarios in section 4,
where the single-device constraint is imposed by physics (cost,
mass, power, volume, sealing of the platform), and where fail-stop
on a long-duration unattended mission is itself a mission failure.

**BEAMFS occupies this specific gap by design.** It does not
attempt to replace ext4, btrfs, ZFS, dm-verity, or dm-integrity in
their respective domains. It complements them as the read-write
data partition layer in a hardened embedded Linux system, as
described in `system-architecture.md`.

---

## 6. Implications for BEAMFS architecture

This threat model has direct, normative consequences for design
decisions. Each consequence is stated as a constraint that the
architecture must satisfy; the rationale references the section
above.

### 6.1 Universal data block protection (no opt-in)

**Constraint.** Reed-Solomon FEC, when applied to data blocks,
must apply to all data blocks of a mounted BEAMFS volume by
default. A per-inode opt-in flag (such as a hypothetical
`BEAMFS_INODE_FL_RS_ENABLED` controlling data block protection) is
rejected.

**Rationale.** Section 2.2, point 2: an adversary will not
restrict attacks to files marked as protected. Section 2.2,
point 3: a flag readable from the same device is not a robust
gating mechanism under an event that may corrupt the flag itself.

**Consequence.** Designs in which RS data protection is gated by an
inode flag are not viable as the long-term architecture. They may
exist transiently as opt-in pilots during incremental rollout, but
the target architecture protects all data blocks unconditionally.

### 6.2 Burst tolerance through stripe geometry

**Constraint.** The on-disk format must allow parity to be
distributed across non-contiguous physical regions, so that a
localized burst affecting N adjacent physical sectors does not
exceed the correction capacity of any single codeword.

**Rationale.** Section 2.2, point 1.

**Consequence.** A naive in-block sub-RS layout (16 sub-blocks of
RS(255,239) packed inside one 4 KiB block) is sufficient for
Family A but insufficient for Family B at the scale of bursts that
exceed 256 bytes. The viable architectures are either out-of-band
shadow regions with deliberate stride placement, or extent-based
layouts where the parity attribute of an extent can specify a
distribution pattern.

### 6.3 Metadata correction, not only detection

**Constraint.** Critical metadata structures — superblock, inode
table, allocation bitmap, directory blocks — must be correctable,
not only detectable. Currently the bitmap block has RS protection
(implemented), the inode has optional RS via reserved bytes
(implemented but per-inode opt-in), and the superblock has only
CRC32 (detection only).

**Rationale.** Section 2.2, point 3.

**Consequence on roadmap.** The unconditional inode RS protection
and the superblock RS protection (likely via a backup superblock
and/or per-superblock RS in reserved padding) are required before
the architecture can claim Family B coverage. These items are not
currently called out as Must-have in `roadmap.md`; this document
elevates them.

### 6.4 Electromagnetic Resilience Journal

**Constraint.** The on-disk event journal (named the **Electromagnetic
Resilience Journal** in v4 of the format; previously "Radiation Event
Journal" in v1–v3 nomenclature) must record sufficient information
across three regimes:

1. **Multi-symbol corrections** (n_positions ≥ 2): per-entry timestamp
   at nanosecond precision, per-entry Shannon entropy estimate over the
   corrected positions. The entropy is the v4 forensic discriminator
   between Family A (uniform position distribution, high entropy) and
   Family B (clustered or polarised distribution, lower entropy).
2. **Single-sample corrections** (n_positions == 1): timestamp recorded,
   entropy explicitly cleared (single sample is mathematically defined
   but not forensically significant). Family A vs B discrimination on
   these entries relies on temporal clustering across the journal.
3. **Uncorrectable events** (n_positions == 0): timestamp and block
   number recorded, the `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE` flag is
   set. These entries identify the saturation boundary (sec 2.3) and
   are strictly more informative than correctable entries for
   capacity planning and end-of-life analysis.

**Rationale.** Section 2.2 point 4 (Family A vs B discrimination) and
section 2.3 (saturation boundary observability).

**Consequence.** All three regimes are implemented in v4 and
documented as the on-disk contract in `Documentation/format-v4.md`
section 6. The Shannon entropy field, formerly classified as Nice-to-
Have in `roadmap.md`, is reclassified as Must-have for Family B
coverage and is present in v4. The UNCORRECTABLE flag is the v4
addition that surfaces the saturation boundary to userspace forensic
tooling without on-disk format breakage (it occupies a previously
reserved bit of `re_flags`).

### 6.5 Bounded auditable code size

**Constraint.** The total auditable kernel-side code must remain
under 5000 lines, as stated in Kconfig. This is a certification
constraint (DO-178C, ECSS-E-ST-40C, IEC 61508), not a stylistic
preference.

**Rationale.** A filesystem that cannot be certified cannot be
deployed on certified platforms. Section 4.1, 4.2, 4.3, 4.4 all
involve certification regimes where ext4 (~100k LOC) and btrfs
(~200k LOC) are non-starters.

**Consequence.** Every architectural choice is evaluated against
its LOC cost. Current count: 2719 LOC. Available margin: ~2280
LOC for all remaining roadmap items combined.

### 6.6 Recovery is in-kernel, in-place, on the IO path

**Constraint.** RS correction occurs synchronously on the read
path, in the kernel, before data is returned to userspace. No
userspace daemon, no FUSE, no offline recovery tool is required
for the steady-state operation.

**Rationale.** Sections 4.1, 4.3: long unattended missions cannot
depend on operator intervention or userspace tooling for correction
of routine SEU.

**Consequence.** rsbep / blkar / SeqBox-style userspace approaches
are unsuitable as the primary mechanism, regardless of their
correction capacity. They may serve as offline forensic recovery
tools complementing BEAMFS, not replacing it.

---

## 7. Relationship with the certification regimes

| Regime           | Domain               | Code auditability requirement | BEAMFS posture |
|------------------|----------------------|-------------------------------|---------------|
| DO-178C          | Civil avionics       | All software fully analyzed at the appropriate level (A through E) | Designed under 5000 LOC to make analysis tractable |
| ECSS-E-ST-40C    | European space software | Process-based, traceability mandatory | Per-correction journal provides operational traceability |
| IEC 61508        | Industrial functional safety | Systematic capability documented | Threat model and verification strategy aligned |
| MIL-STD-882E     | U.S. military system safety | Hazard tracking, failure mode analysis | Family B explicitly enumerated above |
| Common Criteria  | Security evaluation  | Functional + assurance components | Out of current scope; relevant only with future PQ extension |

BEAMFS targets being **suitable for inclusion** in systems undergoing
these evaluations. It does not pre-certify itself; certification is
necessarily system-level and deployment-specific.

---

## 8. Document maintenance

This threat model is normative. Changes require:

1. A documented rationale referencing observed deployment evidence
   or peer-reviewed analysis.
2. A re-evaluation of section 6 constraints, with explicit
   identification of any constraint relaxed or tightened.
3. A note in `roadmap.md` if the change reclassifies any roadmap
   item between Must-have, Nice-to-have, or Long-term.

Editorial corrections (typos, formatting, dead links) do not
require this process.

---

## 9. References

The references below were either consulted directly or are the
canonical primary sources for claims made in this document.
Inclusion does not imply endorsement of any particular conclusion
beyond the specific point cited.

**Scientific lineage and Family A baseline (radiation-tolerant filesystems).**
- Fuchs, Langer, Trinitis. *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.* ARCS 2015, LNCS 9017. https://www.cfuchs.net/chris/publication-list/ARCS2015/FTRFS.pdf — BEAMFS extends this work from a radiation-only threat model to the unified electromagnetic resilience model of section 2.
- *When Radiation Meets Linux: Analyzing Soft Errors in Linux on COTS SoCs under Proton Irradiation.* arXiv 2503.03722.

**Empirical falsification tooling for the saturation boundary (sec 2.3).**
- RadFI — Reed-Solomon Adversarial Fault Injection. Companion repository, v0.1.0, 2026-04-28. Drives a BEAMFS image into the saturation regime under controlled parameters; falsified BEAMFS v1 Theorem IV.1 on data blocks. Not for upstream (dual-use); methodology fully documented for reproducibility.

**On adversarial EM and storage.**
- Far, Qazani, Rad. *Emp-secure data storage through biohybrid and neuromorphic paradigms.* Discover Applied Sciences, 2026. DOI 10.1007/s42452-026-08627-9.
- SemiVision Research. *Drones and EMP Weapons: The Convergence and Strategic Implications for Future Battlefields.* Substack, February 2026.
- Military Embedded Systems. *Hardening flash storage for ultimate data security.* March 2026.

**On existing Linux storage stack capabilities.**
- Linux kernel documentation. dm-verity. https://docs.kernel.org/admin-guide/device-mapper/verity.html
- Linux kernel documentation. EDAC framework. https://www.kernel.org/doc/html/latest/driver-api/edac.html
- Dan Luu. *Files are hard.* http://danluu.com/file-consistency/ and *Filesystem error handling.* http://danluu.com/filesystem-errors/

**On the embedded and physical-AI market trajectory in 2026.**
- Embedded World 2026 reports (Hackster, Electronic Design, Moor Insights & Strategy, ST Blog).

---

## 10. Document status

| Version | Date       | Author                | Change |
|---------|------------|-----------------------|--------|
| 0.1     | 2026-04-25 | Aurelien DESBRIERES   | Initial draft. Establishes Family A/B distinction, gap analysis, design constraints. Reclassifies inode RS unconditional and Shannon entropy as Must-have for Family B coverage. |
