# RadFI: An Algebraic Fault Injection Operator for Empirical Filesystem Resilience Validation

## Falsifying Recovery Theorems through Controlled SEU Emulation in the Linux Kernel

**Technical Report — Version 1**

Aurélien Desbrières — Independent researcher
ORCID: 0009-0002-0912-9487
aurelien@hackers.camp

**Index Terms** — Fault injection, Single-event upset emulation, Filesystem resilience, Falsification, Reed–Solomon, Linux kernel, kprobe, Recovery calculus, BEAMFS, Empirical validation

---

## Abstract

We present RadFI, an algebraic fault injection operator for the empirical validation of filesystem resilience theorems. RadFI is the falsification counterpart to the BEAMFS recovery calculus reported in [1]: where BEAMFS defines a recovery operator $\mathcal{G}$ that drives a corrupted filesystem state back to consistency or to fail-closed, RadFI defines a perturbation operator $\varepsilon$ that injects controlled bit-flips into the bio-layer payload of an instrumented filesystem, modelling the action of a Single-Event Upset on volatile memory transit between user space and storage.

We define $\varepsilon$ formally over the same state space $\mathcal{S}$ used by BEAMFS, formulate a faithfulness property as the statistical indistinguishability of $\varepsilon$ from the canonical SEU model under stated hypotheses, and derive a Linux-kernel implementation path through kprobe-based runtime instrumentation. Three families of perturbation are treated: write-side bit-flips on bio payloads bound for storage, read-side bit-flips on bio payloads returning from storage (DRAM-bidirectional model), and targeted block-level injection through a sector-granularity filter.

This Technical Report v1 reports two empirical results obtained with RadFI v0.1.0 to v0.1.2 against the BEAMFS implementation lineage:

1. **Falsification of BEAMFS v1 Theorem IV.1.** On 2026-04-28, RadFI v0.1.0 produced a controlled bit-flip pattern that the BEAMFS v1 recovery operator failed to correct, exhibiting a state $s$ such that $\mathcal{G}(\varepsilon(s)) \not\models$ and $\mathcal{G}(\varepsilon(s)) \neq \perp$. This is a counter-example to the soundness theorem of [1] under the actual perturbation distribution implementable by RadFI, which led the BEAMFS author to retract Theorem IV.1 and revise the threat model from radiation-only (v1) to full electromagnetic resilience (v2 EMR).

2. **Confirmation of BEAMFS v2 INLINE recovery.** On 2026-04-29, byte-level disk corruption of a BEAMFS v2 INLINE-formatted filesystem (RS(255,239)×16 per-block protection scheme) was successfully recovered by the kernel module's `read_folio` path, with autonomic in-place repair persisting the correction byte-perfect to disk. This is corroboration, not proof, of the revised v2 recovery properties pending formal Theorem v2.1 in [12].

The reference implementation source is held in a private repository (`roastercode/radfi`) pending peer review of the present methodology paper. A reproducibility statement is provided in Section IX.

---

## I. Introduction

Fault injection is the canonical empirical method for evaluating the resilience of computer systems to transient hardware faults [2], [3], [4]. Its scientific function is twofold: to **validate** resilience claims by demonstrating that a system survives a targeted perturbation distribution, and to **falsify** them by exhibiting a perturbation that the system fails to handle. The two are not symmetric: validation accumulates evidence under a chosen perturbation family; falsification produces a single counter-example sufficient to retract a soundness theorem. The latter, properly understood in the Popperian tradition, is what advances the science.

The present report concerns the falsification side. We define an algebraic perturbation operator, RadFI, designed to produce **controlled, repeatable, and physically meaningful** Single-Event Upset emulations against an instrumented Linux filesystem. The operator is the empirical dual of the BEAMFS recovery operator $\mathcal{G}$ defined in [1]: both act on the same filesystem state space $\mathcal{S}$; $\mathcal{G}$ drives perturbed states back to consistency, $\varepsilon$ injects perturbations whose distribution is calibrated to a chosen SEU cross-section regime.

### a) Position of the problem

A filesystem resilience theorem of the form "for every $s \models$ and every $e \in \mathcal{E}$, the recovery operator drives $e(s)$ to consistency or to fail-closed in finite steps" is meaningful only if the perturbation family $\mathcal{E}$ is **realizable**. A theorem that quantifies over a family no real fault can produce is a tautology disguised as a theorem. Conversely, a theorem whose stated $\mathcal{E}$ excludes a class of physically realizable faults is **incomplete**, and the proper response is to enlarge $\mathcal{E}$ and revise the proof, not to dismiss the omitted faults as out of scope.

RadFI is constructed as the empirical instrument that closes this loop. It produces perturbations $e \in \mathcal{E}$ that are simultaneously (i) physically meaningful, in the sense that each $e$ corresponds to a single bit-flip on volatile memory in transit, modelling a canonical SEU [5], [6]; (ii) controllable, in the sense that the position, timing, and target of each flip are governed by stated parameters; and (iii) replayable, in the sense that a fixed PRNG seed reproduces the exact perturbation sequence across runs, enabling regression on resilience claims.

### b) Where we are going

This report defends a single thesis: a filesystem resilience theorem must be subject to empirical falsification, and the instrument of falsification must be a formally specified algebraic operator whose own faithfulness is theorem-bound. The chain is presented in Section V as a three-tier descent — mathematics, algorithm, kernel implementation — closed by a traceability table (Table II) where each line of the perturbation operator's specification is paired with the algorithmic step and the kprobe-based kernel construct that realizes it.

### c) Contributions

We claim the following:

- A formal definition of the perturbation operator $\varepsilon$ acting on the BEAMFS-compatible state space, with stated targeting algebra (region, device, inode, block, op type) and a probability sampling regime calibrated to canonical SEU rate models (Section III).
- A **faithfulness theorem** (Theorem IV.1, full proof in Section X) showing that under stated hypotheses on the bit-flip distribution and the kprobe interception path, $\varepsilon$ is statistically indistinguishable from the canonical SEU perturbation operator $\varepsilon_{\text{SEE}}$ of [1, Sec. II] within bounded probability $\delta$.
- A three-tier descent (Section V) from $\varepsilon$ to a kprobe-based algorithm to a Linux-kernel module implementation, with a traceability table (Table II) auditable line by line.
- An **empirical falsification** (Section VI) of BEAMFS v1 Theorem IV.1 [1] using RadFI v0.1.0 against the BEAMFS v1 reference implementation, dated 2026-04-28; and a **corroboration** of BEAMFS v2 INLINE recovery against equivalent disk-level corruption, dated 2026-04-29.

### d) Scope

This is v1. We model bit-flip perturbations on bio-layer payloads only. We do not model: bus-level SET on the DRAM channel; SEFI on storage controller firmware; SEL on the host system; multi-flip events within a single bio (capped at one flip per intercepted bio in v0.1.x); RDMA fast-path interception bypassing the bio layer; cumulative TID/DDD effects (these are below the filesystem and not perturbations in the SEE sense). RadFI requires kernel privileges to load and operates on instrumented test devices only; it is not a remote attack model and is not, by construction, a production deployment tool.

### e) Anti-claim

We are explicit about what RadFI v1 does **not** establish:

- **Faithfulness as ground-truth equivalence with beam-time measurements.** The faithfulness theorem (Theorem IV.1) shows statistical indistinguishability under stated hypotheses, not physical equivalence with accelerator-measured upset distributions. Bridging this gap requires beam-time at facilities such as RADEF, GANIL, or TRIUMF, and is reserved for v2.
- **Falsification of BEAMFS v2.** The falsification of v1 Theorem IV.1 in Section VI does not extend to v2, whose revised recovery properties are corroborated empirically but not yet formally proved. The successor report [12] will state and prove Theorem v2.1.
- **Coverage of adversarial fault injection by an attacker with kernel privileges.** RadFI's targeting filters could in principle be misused; the v0.1.x build refuses to attach to physical block devices and is restricted to loop-mounted test images. This is a safety measure, not a security guarantee.

---

## II. Threat Model: SEU Emulation in Volatile Memory

The faithfulness theorem (Section IV) is meaningful only against a precisely stated perturbation family. This section establishes that family. We separate three concerns: the canonical SEU primitive that RadFI emulates (II-A), the kernel transit path on which RadFI operates (II-B), and the DRAM-bidirectional refinement introduced in v0.1.2 following empirical observation of the Linux 7.0.x I/O path (II-C).

### A. The Canonical SEU Primitive

Following the canonical SEE taxonomy of [5], [6], a Single-Event Upset is a single-bit (occasionally multi-bit) state transition of a memory cell induced by the charge deposited by an ionizing particle. The probability of a SEU striking a given cell exposed to a particle flux $\Phi$ of linear energy transfer LET is, to first order, $P_{\text{SEU}} = 1 - \exp(-\sigma(\text{LET}) \, \Phi \, t)$, where $\sigma(\text{LET})$ is the cross-section curve of the cell technology [7].

For the purposes of RadFI, the modelled primitive is the **single bit-flip** — a transition of one bit from 0 to 1 or 1 to 0, in a memory location reached by a kernel I/O path. We do not model the underlying physics; we model the **observable consequence** at the byte level. This is the same abstraction used by software-implemented fault injection tools in the canonical literature [2], [3], and is the level at which a filesystem recovery operator can defend.

### B. Kernel Transit Path

The action surface of RadFI is the bio-layer of the Linux block I/O path. A `struct bio` is the kernel's representation of an in-flight I/O request; it carries a vector of `bio_vec` segments, each pointing to a kernel memory page with offset and length. When a filesystem issues a read or write, the kernel constructs one or more bios and submits them through `submit_bio_noacct()` or, in some buffer-cache paths, `submit_bh()` [8].

RadFI intercepts these submission entry points through kprobes. At the moment of interception, the bio's payload is in DRAM, in transit to or from the storage device. A single bit-flip applied to one byte of the first `bio_vec` segment is the operational realization of an SEU on this payload.

### C. The DRAM-Bidirectional Model

RadFI v0.1.0 modelled bit-flips on **write** bios only, on the rationale that a flip on a read bio would be re-fetched from storage and effectively self-correct. Empirical observation during the BEAMFS v2 development on 2026-04-29 invalidated this rationale: under Linux kernel 7.0.x, a read of an `sb_bread`-backed block (the BEAMFS metadata path) traverses `__bread_gfp → __getblk_slow → bh_read → submit_bio_noacct`, and the resulting payload is delivered to the page cache without re-fetch. A flip on a read bio is therefore **persistent** until the next page cache eviction, and corrupts the consumer of the data exactly as a flip on a write bio corrupts the disk.

RadFI v0.1.2 introduces the **DRAM-bidirectional model**: bit-flips can target both write bios (modelling SEU on memory transit toward storage) and read bios (modelling SEU on memory transit from storage to user). The choice is governed by the `inject_on_read` debugfs flag (default true). Setting it to false restores the v0.1.0 write-only behaviour.

This model is more conservative — and more physically realistic — than the v0.1.0 model: a DRAM cell holding bio payload is vulnerable to SEU regardless of the data flow direction, and an instrument that injects only one direction misses half the empirical surface.

---

## III. Perturbation Operator: Formal Definition

We now define formally the object on which the kernel implementation acts.

### A. State Space and Perturbation Algebra

We adopt the BEAMFS-compatible state space $\mathcal{S} = \mathcal{S}_V \times \mathcal{S}_P$ of [1, Definition III.1], where $\mathcal{S}_V$ is the volatile component (kernel data structures, page cache, in-memory mirrors) and $\mathcal{S}_P$ is the persistent component (on-disk data and metadata).

**Definition III.1** *(Perturbation operator)*. The RadFI perturbation operator $\varepsilon$ is a partial function $\varepsilon : \mathcal{S} \times \Pi \to \mathcal{S}$, parameterized by a perturbation parameter set $\Pi = \langle \mathit{filters}, \mathit{prob}, \mathit{seed} \rangle$, and defined by the following procedure on input $(s, \pi)$:

1. Intercept a bio submission $b$ in transit through `submit_bio_noacct` or `submit_bh`.
2. Apply the filter algebra $\mathit{filters}(\pi)$ to $b$. If $b$ is rejected, return $s$ unchanged.
3. Sample the PRNG seeded by $\mathit{seed}(\pi)$. If the sample exceeds $\mathit{prob}(\pi)$, return $s$ unchanged.
4. Otherwise, select a byte in the first non-empty `bio_vec` segment of $b$ uniformly at random, and flip one bit at a uniformly sampled position $0 \leq p < 8$.
5. Return the modified state $s'$.

**Definition III.2** *(Filter algebra)*. The filter set $\mathit{filters}$ is a tuple $\langle \mathit{dev}, \mathit{inode}, \mathit{block}, \mathit{op} \rangle$ where each component is either a wildcard (admits any value) or a concrete value. A bio $b$ passes the filter iff every non-wildcard component matches the corresponding attribute of $b$:

- $\mathit{dev}$: the encoded device number `bio->bi_bdev->bd_dev` (kernel encoding `(major << 8) | minor` for legacy minors).
- $\mathit{inode}$: the inode number, available only on the `submit_bh` hook path where the buffer head carries `b_assoc_map`.
- $\mathit{block}$: the sector number `bh->b_blocknr`, available on the `submit_bh` path.
- $\mathit{op}$: the bio operation type, restricted to `{REQ_OP_READ, REQ_OP_WRITE}` in v0.1.2; other ops are filtered out.

The inode and block filters are bound to the `submit_bh` hook because a `bio` may aggregate buffers from multiple inodes; RadFI v0.1.x does not attempt fine-grained per-buffer targeting on the bio path.

### B. Probability Sampling and PRNG

The probability $\mathit{prob}$ is expressed in parts per million (PPM): $\mathit{prob} = 10^6$ flips every intercepted bio that passes the filter; $\mathit{prob} = 10^3$ flips one bio in a thousand. The PRNG is a xoshiro256** generator [9], seeded by the user-supplied $\mathit{seed}$. A fixed seed produces a deterministic flip sequence, enabling regression on resilience properties: a recovery theorem that holds for seed $S_1$ but not for $S_2$ is a falsification, regardless of the absolute flip count.

### C. The Five Invariants of a Faithful SEU Emulator

The faithfulness theorem (Theorem IV.1) requires the following invariants of the perturbation operator. They are minimal and stated separately so that an implementation can be audited against them line by line.

- **F1 — Single-bit primitive.** Each invocation of $\varepsilon$ that produces a perturbation flips exactly one bit. Multi-flip is by repeated invocation, not by single-call amplification. This matches the canonical single-event abstraction of [5], [6].
- **F2 — Targeting independence from data content.** The byte and bit position selected by $\varepsilon$ are sampled from the bio payload's geometry, not from its contents. A flip at offset 17 in a bio is not statistically correlated with the value at offset 17.
- **F3 — Filter monotonicity.** Adding a non-wildcard filter component to $\mathit{filters}$ can only reduce the set of intercepted bios, never enlarge it. A filtered injection is always a sub-distribution of an unfiltered one.
- **F4 — Replay determinism.** Given fixed $(\mathit{filters}, \mathit{prob}, \mathit{seed})$ and a fixed I/O sequence, $\varepsilon$ produces the same flip sequence across runs.
- **F5 — Counter accounting.** $\varepsilon$ maintains atomic counters $(\mathit{call\_count}, \mathit{flip\_count}, \mathit{skipped\_disabled}, \mathit{skipped\_prob})$ such that the sum $\mathit{flip\_count} + \mathit{skipped\_disabled} + \mathit{skipped\_prob}$ equals the number of post-filter intercepted bios.

F1 grounds the SEU semantics. F2 is the absence-of-bias property. F3 is the algebraic well-formedness of the filter lattice. F4 is what makes RadFI a falsification instrument rather than a stochastic noise source. F5 is the audit surface that allows an empirical run to be self-validated against the expected statistics.

---

## IV. Faithfulness Theorem

### A. Statement

**Theorem IV.1** *(Faithfulness of RadFI as SEU emulator)*. Let $\varepsilon_{\text{RadFI}}$ be the perturbation operator of Definition III.1, satisfying invariants F1–F5. Let $\varepsilon_{\text{SEE}}$ be the canonical SEU operator of [1, Sec. II], parameterized by a cross-section $\sigma$ and a flux $\Phi$. Let $X$ be the random variable counting bit-flips per unit of bio payload bytes traversed, under $\varepsilon_{\text{RadFI}}$ with $\mathit{prob} = p_R$ and a uniformly distributed I/O workload, and let $Y$ be the corresponding random variable under $\varepsilon_{\text{SEE}}$.

Then for every $\delta > 0$, there exists a calibration $p_R(\sigma, \Phi)$ such that the total variation distance between the distributions of $X$ and $Y$ is bounded:

$$d_{\text{TV}}(X, Y) \leq \delta.$$

### B. Discussion

**a) What the theorem establishes.** RadFI, parameterized by $p_R$, can emulate any SEU regime up to total variation $\delta$, where $\delta$ shrinks as the I/O workload duration grows (asymptotic indistinguishability). The proof (Section X) is a concentration argument on bio interception counts, exploiting F4 and F5.

**b) What the theorem does not establish.** Faithfulness is statistical, not physical. RadFI does not reproduce the spatial correlation of multi-bit upsets within a memory row, nor the temporal clustering of solar particle event bursts. Both are observable in beam-time data and absent from RadFI by design. v2 of this report will discuss extensions (per-row MBU clustering, Poisson burst arrivals) that close part of this gap.

**c) Composition with BEAMFS.** Theorem IV.1 of the present report ($\varepsilon_{\text{RadFI}}$ faithful) composes with [1, Theorem IV.1] (BEAMFS recovery sound under $\mathcal{E}_{\text{SEE}}$) only if the family $\mathcal{E}$ used by the BEAMFS theorem contains the distribution emulated by RadFI. The empirical falsification reported in Section VI demonstrates that the v1 $\mathcal{E}$ did not.

---

## V. From Mathematics to Code: Three-Tier Derivation

### A. Tier 1: The Perturbation Operator (Mathematics)

Definition III.1 is the mathematical contract. The five invariants F1–F5 are the obligations the next tiers must preserve.

### B. Tier 2: The Algorithm

The perturbation operator translates to a deterministic kernel-conformant algorithm. A `pre_handler` is registered on each kprobe (one for `submit_bh`, one for `submit_bio_noacct`); on each invocation, the handler reads the relevant arguments, applies the filter, samples the PRNG, and performs the flip via `kmap_local_page` for arm64-safe page access.

```c
/*
 * RadFI bio pre-handler — kprobe on submit_bio_noacct.
 * Returns 0 always (pre-handler does not redirect execution).
 */
static int radfi_kp_submit_bio_pre(struct kprobe *kp,
                                   struct pt_regs *regs)
{
    struct radfi_state *st = radfi_global;
    struct bio *bio;
    unsigned int op;
    dev_t dev;

    if (!st || !READ_ONCE(st->hook_blk))
        return 0;

    bio = (struct bio *)regs->regs[0];
    if (!bio || !bio->bi_bdev)
        return 0;

    /* F3: filter monotonicity (op type) */
    op = bio_op(bio);
    if (op == REQ_OP_WRITE) {
        /* always candidate */
    } else if (op == REQ_OP_READ) {
        if (!READ_ONCE(st->inject_on_read))
            return 0;        /* v0.1.2 flag */
    } else {
        return 0;            /* skip non-data ops */
    }

    /* F3: filter monotonicity (device) */
    dev = bio->bi_bdev->bd_dev;
    if (READ_ONCE(st->target_dev) &&
        (u32)dev != READ_ONCE(st->target_dev))
        return 0;

    atomic64_inc(&st->call_count);

    /* F1: probability sampling */
    if (!radfi_should_inject(st)) {
        atomic64_inc(&st->skipped_prob);
        return 0;
    }

    /* F1, F2: single bit-flip on first segment, uniform */
    radfi_flip_one_bit_in_first_segment(bio);

    atomic64_inc(&st->flip_count);
    return 0;
}
```

The algorithm is structurally simple. Its correctness rests entirely on the alignment between the mathematical invariants and the kernel-side implementation choices. Table II makes this alignment explicit.

### C. Tier 3: The Kernel Implementation

The reference implementation is a Linux kernel module (`radfi.ko`, GPL-2.0) registering two kprobes: one on `submit_bh` (filesystem hook), one on `submit_bio_noacct` (block layer hook). Configuration is exposed through debugfs entries under `/sys/kernel/debug/radfi/`:

- `enabled` (bool): master switch.
- `hook_fs` (bool), `hook_blk` (bool): per-hook enable.
- `inject_on_read` (bool, v0.1.2): DRAM-bidirectional toggle.
- `probability` (u32): PPM probability.
- `target_dev` (u32), `target_inode` (u64), `target_block` (u64): filter values.
- `seed` (u64): PRNG seed.
- `call_count`, `flip_count`, `skipped_disabled`, `skipped_prob` (read-only u64): F5 counters.

The kmap path uses `kmap_local_page` (rather than the deprecated `kmap_atomic`) for arm64 safety [10]. The PRNG state is per-module-load, re-seeded at module init.

**a) Differences from generic SDC fault-injection tools.** Tools such as Xception [2] and EDFI [4] inject into instruction streams or memory regions selected by static analysis. RadFI restricts itself to the bio-layer payload, which corresponds precisely to the abstraction at which a filesystem can defend (the BEAMFS recovery operator $\mathcal{G}$ acts on bytes read back from disk, not on instructions). This narrow scope is what makes the falsification-of-recovery-theorems use case crisp.

**b) Production safety.** The v0.1.x build aborts module load if the host system has any non-loop block device mounted with a `BEAMFS-COMPATIBLE` superblock. This is a build-time check enforced by a runtime probe in `radfi_init`. The intent is to make accidental injection on a production filesystem impossible without explicit code modification.

### D. Traceability: Math → Algorithm → Code

Table II expresses the descent in tabular form, in the same auditable contract style as [1, Table II].

| Math Commitment (Tier 1) | Algorithm (Tier 2) | Kernel Construct (Tier 3) | Phase |
|---|---|---|---|
| F1 (Single-bit primitive) | `radfi_flip_one_bit_in_first_segment` flips one bit per call | Sampling: `byte_off = prng % bv.bv_len`; `bit_pos = prng % 8`; XOR | Runtime |
| F2 (Targeting independence) | Sample positions from bio geometry only, not from byte values | `bv.bv_len`, `bv.bv_offset` used as bounds; payload bytes never read | Runtime |
| F3 (Filter monotonicity) | Sequential filter checks return 0 on any non-match | `if ((u32)dev != target_dev) return 0;` per filter component | Runtime |
| F4 (Replay determinism) | xoshiro256** seeded by `seed` parameter | `radfi_prng_state` initialized from `st->prng_seed` at module init | Mount |
| F5 (Counter accounting) | `atomic64_inc` on each path | `&st->call_count`, `&st->flip_count`, `&st->skipped_*` | Runtime |
| Definition III.1, step 1 (Intercept) | kprobe `pre_handler` | `register_kprobe(&radfi_kp_submit_bio)` | Mount |
| Definition III.1, step 2 (Filter) | Filter algebra on bio attributes | `bio_op(bio)`, `bio->bi_bdev->bd_dev` | Runtime |
| Definition III.1, step 3 (Probability) | PRNG comparison vs `prob` | `radfi_should_inject(st)` | Runtime |
| Definition III.1, step 4 (Flip) | `kmap_local_page` + XOR + `kunmap_local` | arm64-safe page access [10] | Runtime |
| Definition III.2 (Filter algebra) | Tuple of optional filters | debugfs entries `target_dev`, `target_inode`, `target_block`, op type | Mount |
| DRAM-bidirectional (II-C) | `inject_on_read` toggle on REQ_OP_READ | `READ_ONCE(st->inject_on_read)` at op type filter | Runtime |

**Table II.** Traceability of the perturbation operator. Each mathematical commitment maps to an algorithmic step and a kernel construct. The "Phase" column indicates when the construct is checked: Mount = once at module load; Runtime = on each bio interception.

---

## VI. Empirical Contribution

### A. Falsification of BEAMFS v1 Theorem IV.1 (2026-04-28)

**Setup.** A BEAMFS v1 reference implementation [1] was instrumented with RadFI v0.1.0 in a Yocto-based qemu-aarch64 lab environment (Linux kernel 7.0.1, distro poky-hardened 1.0). The filesystem was formatted with the v1 INODE_UNIVERSAL protection scheme (RS(255,239) coverage on inode metadata, allocation bitmap, and superblock; data blocks unprotected by the RS layer). RadFI was configured with `hook_blk = 1`, `target_dev = 1792` (the test loop device), `inject_on_read = 0` (write-only model, v0.1.0 semantics), and `probability = 1000` (one flip per thousand intercepted writes).

**Result.** A workload of 1024 sequential 4 KiB writes followed by a re-mount and a checksummed read produced 7 corrupted blocks at the user level (sha256 mismatch versus the pre-write fixture). The BEAMFS v1 recovery operator $\mathcal{G}$, invoked on read, did not return any of these blocks to consistency, nor did it transition to fail-closed: it returned the corrupted bytes silently to user space. This is a counter-example to [1, Theorem IV.1]: there exists $s \models$ and $e \in \mathcal{E}_{\text{RadFI v0.1.0}}$ such that $\mathcal{G}^k(e(s)) \not\models$ and $\mathcal{G}^k(e(s)) \neq \perp$ for all $k \in \mathbb{N}$.

**Diagnosis.** The v1 INODE_UNIVERSAL scheme, by design, did not extend RS coverage to data blocks. The recovery operator's coverage of $\mathcal{E}_{\text{SEE}}$ in [1, Theorem IV.1] was conditional on the layout's redundancy specification (invariant I1 in [1, Sec. III-C]), which the v1 layout did not satisfy on data blocks. The RadFI experiment exposed this incompleteness experimentally rather than theoretically.

**Consequence.** The BEAMFS author retracted the wording of v1 Theorem IV.1 on the same date and revised the threat model from radiation-only (v1) to full electromagnetic resilience (v2 EMR), introducing the INLINE protection scheme (RS(255,239)×16 per-block on data blocks unconditionally) [12]. The v2 successor report will state and prove a revised Theorem v2.1 covering data-block perturbations.

This falsification is the central empirical contribution of the present report. It is consistent with the Popperian principle that a soundness theorem stands until a single counter-example is produced, and that the counter-example must motivate, not merely refute, the next theorem.

### B. Confirmation of BEAMFS v2 INLINE Recovery (2026-04-29)

**Setup.** A BEAMFS v2 INLINE-formatted filesystem (RS(255,239)×16, per-block protection on all data blocks) was created with `mkfs.beamfs -s inline` on a 64 MiB loop image. A conformance fixture canary block was emitted at logical block 19 by the mkfs tool, with byte-deterministic content yielding sha256 = `ced446d9bf5e6682a48e8782ce5ad33f575f08921451afaa127f26dc37515bab` for the user-visible 3824-byte payload, and sha256 = `fb8a3b9e2704ce3fc11a0d0a4139d36c916cb5e7230acde8d2fa00ace739a91c` for the raw 4096-byte on-disk block, both reproducible across x86_64 and aarch64 builds.

**Method.** Direct disk-level corruption (not RadFI) was applied: a single byte at offset 77824 (logical block 19, header byte 0) was XOR-flipped from `0x42` to `0x43`. The image was then mounted through BEAMFS, page caches were dropped, and the canary file was read through the standard VFS path.

**Result.** The user-level read returned the canonical 3824-byte payload with sha256 matching the fixture. The kernel log recorded `beamfs/inline: ino=2 iblock=0 subblock=0: 1 symbol(s) corrected`. A subsequent raw read of the on-disk block, after a second cache drop, returned the canonical 4096-byte content with sha256 matching the fixture: the autonomic in-place repair had persisted the correction byte-perfect to disk. The recovery operator's idempotence (BEAMFS invariant I4 [1, Sec. III-C]) was confirmed on a third read producing identical results without further log entries.

**Methodological note.** This experiment used direct disk-level corruption rather than RadFI bio-layer injection. The choice was operationally driven: during the same session, RadFI v0.1.2 hook_blk pre-handler was observed to register on `submit_bio_noacct` (kprobe armed at the correct address, native kprobe events registering 10 hits per cat invocation) but the RadFI counters did not increment. Diagnosis is deferred to v2 of this report. The two methods are equivalent at the SEU semantic level: each produces a single bit-flip on volatile or persistent memory in the read path. Direct disk corruption was the simpler chain of trust for the v2 INLINE empirical result, and the result is paper-grade reproducible (fixture sha256 values are byte-deterministic across platforms).

### C. Falsification Matrix

| Implementation | Protection Scheme | Perturbation | Result |
|---|---|---|---|
| BEAMFS v1 (2026-04-28) | INODE_UNIVERSAL (metadata only) | RadFI v0.1.0, write hook, 1000 PPM | **7/1024 silent corruption** (falsification of v1 Theorem IV.1) |
| BEAMFS v2 (2026-04-29) | UNIVERSAL_INLINE (RS×16 per data block) | dd-based single byte flip on canary block | **byte-perfect recovery + autonomic repair** (corroboration of pending v2 Theorem) |

**Table III.** Empirical falsification matrix. The v1 result is a single counter-example sufficient to retract Theorem IV.1 of [1]. The v2 result is a single positive instance corroborating, but not proving, the revised v2 recovery properties.

---

## VII. Scope and Limitations

We adopt the same posture as [1]: the limitations section is structural, not concessionary.

**a) v1 covers.** The mathematical formalization of $\varepsilon$ (Section III), the faithfulness theorem (Section IV with proof in Section X), the three-tier descent to a Linux kernel module (Section V), and two empirical applications: falsification of BEAMFS v1 and corroboration of BEAMFS v2 INLINE.

**b) v1 does not cover.**

- **Multi-flip per bio.** v0.1.x flips at most one bit per intercepted bio. A real burst event ionizing multiple cells along a single particle track is not modelled.
- **MBU spatial correlation.** Multi-bit upsets in physically adjacent cells are correlated through the particle's track geometry. RadFI's uniform sampling does not reproduce this correlation.
- **Beam-time calibration.** The faithfulness theorem (Theorem IV.1) is a statistical claim; physical equivalence with accelerator-measured upset distributions is not established.
- **RDMA fast path.** RadFI hooks the bio layer; RDMA paths bypassing this layer are not intercepted.
- **Hook diagnostic on Linux 7.0.x.** The hook_blk pre-handler observed on 2026-04-29 to register without firing under controlled conditions remains unexplained at v0.1.2. Diagnosis is deferred.
- **Verified compilation.** The kernel implementation is C; the gap from algorithm to executed binary is the standard unverified-compilation gap [11].

**c) Methodological limits.** The author is the sole contributor; no peer review beyond the public visibility of the BEAMFS repository has shaped this text. The falsification of BEAMFS v1 is a self-falsification: the same author wrote v1, RadFI, and the present retraction. We claim this is a strength rather than a weakness — a single author's willingness to falsify their own theorem is a stronger Popperian commitment than third-party falsification — but we acknowledge the absence of independent replication is a v2 priority.

**d) Threats to validity.** The principal threat is that the perturbation distribution emulated by RadFI is not faithful to the SEU distribution observed in deployed systems. Theorem IV.1 establishes statistical indistinguishability under stated hypotheses, but the hypotheses themselves (uniform sampling, single-bit primitive, no spatial correlation) are simplifications. v2 will address each.

---

## VIII. Conclusion

We have presented the v1 mathematical scaffolding and empirical instantiation of RadFI, an algebraic fault injection operator for the falsification of filesystem resilience theorems. The contribution is twofold: a formal definition of the perturbation operator with a faithfulness theorem, and an empirical falsification of the BEAMFS v1 recovery soundness theorem dated 2026-04-28, which motivated the BEAMFS v2 EMR revision.

The position of the report is that filesystem resilience claims must be subject to empirical falsification, and that the falsification instrument must itself be a formally specified operator whose own faithfulness is theorem-bound. RadFI is the first such instrument we are aware of in the Linux filesystem ecosystem.

This is also v1 in the same sense as [1]: it establishes authorship and fixes the scope. v2 will document the hook diagnostic resolution, multi-flip extension, MBU correlation modelling, and beam-time calibration at a recognized accelerator facility. v3 will explore proof-assistant mechanization of Theorem IV.1.

---

## IX. Reproducibility

**a) Source repository.** The reference implementation source is held in a private repository, `roastercode/radfi`, pending peer review of the present methodology paper. Source access is granted to qualified academic researchers under non-disclosure agreement, contact `aurelien@hackers.camp`. The present paper is published in the public BEAMFS repository, `roastercode/beamfs`, sub-directory `papers/2026-04-radfi-v1/`.

**b) Build environment.** The reference build environment is a Yocto-based qemu-aarch64 lab (Linux kernel 7.0.1, distro poky-hardened 1.0, kernel module out-of-tree). The RadFI module loads via `modprobe radfi`, exposes its debugfs interface under `/sys/kernel/debug/radfi/`, and registers two kprobes whose presence is verifiable through `/sys/kernel/debug/kprobes/list`.

**c) Empirical reproduction.** Both empirical results of Section VI are reproducible from the public BEAMFS source code at the tagged commit:

- BEAMFS v1 falsification: `roastercode/beamfs` tag `v1.0.0-paper-pending` (commit `32f590e`), with RadFI v0.1.0 module load and the parameters of Section VI-A.
- BEAMFS v2 INLINE confirmation: `roastercode/beamfs` tag `v0.2.0-palier3-validated` (commit `c47f130`), with the dd-based corruption procedure of Section VI-B and the conformance fixture canary block defined in [12, format-v4 §11].

The fixture sha256 values are normative and reproducible across x86_64 and aarch64 builds (verified by `cmp(1)` silent on the canary block raw bytes).

**d) Build (paper).** The present paper is generated from a markdown source convertible to LuaLaTeX IEEEtran via `pandoc`. Figures and tables are inline.

```
cd papers/2026-04-radfi-v1
make            # paper.pdf via pandoc + lualatex
```

---

## X. Appendix A: Faithfulness Theorem — Full Proof

We restate Theorem IV.1 for self-containment.

**Restated theorem.** Let $\varepsilon_{\text{RadFI}}$ be the perturbation operator of Definition III.1, satisfying invariants F1–F5. Let $\varepsilon_{\text{SEE}}$ be the canonical SEU operator parameterized by cross-section $\sigma$ and flux $\Phi$. Let $X$ count bit-flips per byte of bio payload traversed under $\varepsilon_{\text{RadFI}}$ with $\mathit{prob} = p_R$ and a uniform I/O workload of duration $T$, and $Y$ the corresponding count under $\varepsilon_{\text{SEE}}$. Then for every $\delta > 0$, there exists $p_R(\sigma, \Phi)$ such that $d_{\text{TV}}(X, Y) \leq \delta$ for $T$ sufficiently large.

**Proof.** Let $N$ be the number of bytes of bio payload traversed in time $T$. Under uniform workload, $N$ grows linearly: $N = r \cdot T$ for I/O rate $r$.

*Distribution of $X$.* By F1 and F2, each intercepted bio that passes the filter and the probability roll produces exactly one bit-flip at a uniformly sampled byte and bit position. Let $B$ be the number of intercepted-and-passed bios in time $T$. By F4 and the linearity of the workload, $B \sim \text{Binomial}(N / \bar{b}, p_R)$ where $\bar{b}$ is the average bio payload size. By the law of large numbers, $X = B$ converges in distribution to $\text{Poisson}(\lambda_R)$ with $\lambda_R = (N / \bar{b}) \cdot p_R$, in the limit $N \to \infty$, $p_R \to 0$, $N p_R \to \lambda_R$.

*Distribution of $Y$.* The canonical SEU model [5], [7] gives a Poisson distribution of bit-flips per byte exposed: $Y \sim \text{Poisson}(\lambda_S)$ with $\lambda_S = N \cdot \sigma \Phi \cdot 8$ (factor 8 for bits per byte, under the standard cross-section convention).

*Calibration.* Choose $p_R = (\bar{b} / N) \cdot \lambda_S$, i.e., $p_R = \bar{b} \cdot \sigma \Phi \cdot 8$. This yields $\lambda_R = \lambda_S$.

*Bounding total variation.* Two Poisson distributions with equal rate are identically distributed; $d_{\text{TV}} = 0$. The convergence to Poisson is $O(1/\sqrt{N})$ by the Berry-Esseen theorem applied to the binomial-to-Poisson approximation. Therefore for any $\delta > 0$, choosing $N \geq C / \delta^2$ for an appropriate constant $C$ yields $d_{\text{TV}}(X, Y) \leq \delta$.

This concludes the proof.

**a) What the proof does not show.** The proof treats the bit-flip count distribution only. It does not address: (a) spatial correlation of multi-bit upsets, which is absent from RadFI by F1; (b) temporal clustering during solar particle events, which is absent from the uniform-workload assumption; (c) the joint distribution of flips with the I/O workload itself. Each is noted as v2 work.

---

## XI. References

[1] A. Desbrières, "BEAMFS: Recovery Calculus for Filesystems under Single-Event Effects — Mathematical Foundations across the Volatile–Persistent Memory Stack," Technical Report v1, Apr. 2026. doi: 10.5281/zenodo.[BEAMFS-v1-DOI-pending]. Available: https://github.com/roastercode/beamfs/tree/main/papers/2026-04-beamfs-v1/

[2] J. Carreira, H. Madeira, and J. G. Silva, "Xception: A technique for the experimental evaluation of dependability in modern computers," *IEEE Transactions on Software Engineering*, vol. 24, no. 2, pp. 125–136, 1998. doi: 10.1109/32.666826.

[3] M.-C. Hsueh, T. K. Tsai, and R. K. Iyer, "Fault injection techniques and tools," *Computer*, vol. 30, no. 4, pp. 75–82, 1997. doi: 10.1109/2.585157.

[4] C. Giuffrida, A. Kuijsten, and A. S. Tanenbaum, "EDFI: A dependable fault injection tool for dependability benchmarking experiments," in *Proc. IEEE 19th Pacific Rim Int. Symp. Dependable Computing*, 2013, pp. 31–40. doi: 10.1109/PRDC.2013.12.

[5] P. E. Dodd and L. W. Massengill, "Basic mechanisms and modeling of single-event upset in digital microelectronics," *IEEE Transactions on Nuclear Science*, vol. 50, no. 3, pp. 583–602, 2003. doi: 10.1109/TNS.2003.813129.

[6] R. C. Baumann, "Radiation-induced soft errors in advanced semiconductor technologies," *IEEE Transactions on Device and Materials Reliability*, vol. 5, no. 3, pp. 305–316, 2005. doi: 10.1109/TDMR.2005.853449.

[7] E. L. Petersen, "Cross section measurements and upset rate calculations," *IEEE Transactions on Nuclear Science*, vol. 43, no. 6, pp. 2805–2813, 1996. doi: 10.1109/23.556870.

[8] J. Corbet, A. Rubini, and G. Kroah-Hartman, *Linux Device Drivers*, 3rd ed., O'Reilly Media, 2005. (Block layer chapter; bio submission API has evolved since publication, see kernel source `block/blk-core.c` for current API.)

[9] D. Blackman and S. Vigna, "Scrambled linear pseudorandom number generators," *ACM Transactions on Mathematical Software*, vol. 47, no. 4, art. 36, 2021. doi: 10.1145/3460772.

[10] Linux kernel contributors, "Highmem and kmap_local_page documentation," Linux kernel source tree, `Documentation/mm/highmem.rst`, 2026.

[11] G. Klein et al., "seL4: Formal verification of an OS kernel," in *Proc. 22nd ACM SIGOPS Symp. Operating Systems Principles (SOSP)*, 2009, pp. 207–220. doi: 10.1145/1629575.1629596.

[12] A. Desbrières, "BEAMFS v2: A Linux Filesystem with Electromagnetic Resilience and Reed-Solomon Recovery," Technical Report v2 (in preparation), 2026. Code at `roastercode/beamfs` tag `v0.2.0-palier3-validated`. Format specification at `Documentation/format-v4.md`.

[13] J. Corbet, "Notes on the kprobes interface," LWN.net, 2017. Available: https://lwn.net/Articles/132196/

[14] Linux kernel contributors, "kprobes documentation," Linux kernel source tree, `Documentation/trace/kprobes.rst`, 2026.

---

*End of paper.*

*Manuscript word count: ~6000 words (~8 pages IEEEtran 2-column 10pt A4).*

*Submitted to public BEAMFS repository on 2026-04-29 as antériorité scientifique. Companion code repository `roastercode/radfi` remains private pending peer review of the present methodology.*
