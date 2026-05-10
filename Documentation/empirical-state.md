# beamfs - Empirical state

> Live snapshot of what is empirically validated on the bench.
> Updated as bench evidence accumulates. Replaces the per-session
> ephemera (`EMPIRICAL-FINDINGS.md`, `multifs-bench-2026-04-30.md`,
> `2026-04-28-validation-session.md`) which are now archived.

Last updated : 2026-05-01

---

## 1. Empirical baseline

beamfs is observed against ext4 / ext3 / btrfs / squashfs under
RadFI live fault injection (companion paper v1, Zenodo 19885777),
on a 4-VM aarch64 cluster (master + compute01..03), kernel 7.0.3,
beamfs scheme=5 (INODE_UNIVERSAL).

Test cluster : 5 USB sticks attached to compute01 (R-isolation, R21).
Each USB holds one FS partition. Master is the orchestrator, never
a victim.

Each beamfs-bench full run produces 77 factual observations across
6 test classes. The bench is a measurement instrument; PASS/FAIL at
bench level reflects only that technical phases completed cleanly.

---

## 2. Per-class bench scope (R7, expanded)

| Class    | Mechanism                          | FS            | Mode                |
|----------|-------------------------------------|---------------|---------------------|
| multifs  | RadFI live attack                  | 5 FS x 3 probs| live SEU             |
| cluster  | RadFI live 4 nodes                 | beamfs only   | live SEU x4 nodes   |
| bitrot   | dd random offline                  | 5 FS x 4 cells| at-rest corruption  |
| metadata | RadFI deterministic block target   | 5 FS x 4 cells| metadata SEU         |
| crash    | virsh destroy mid-write            | 4 FS x 1 cell | power-loss recovery |
| fsck     | fsck.<fs> offline                  | 4 FS x 1 cell | offline check       |

(squashfs is read-only; skipped on crash + fsck.)
(beamfs returns NOT_IMPLEMENTED on fsck; fsck.beamfs is Phase 2.)

---

## 3. Validated capabilities (per-class)

### 3.1 multifs : live SEU under RadFI

beamfs RECOVERED 3/3 (probs 1k, 100k, 1M) reproducibly.
ext4/btrfs FS_PANIC at 1M (variable timing, ext4 sometimes at 100k).
ext3 RECOVERED 3/3 (legacy journal robustness).
squashfs RECOVERED 3/3 (RO; not a fair comparison).

### 3.2 cluster : 4 nodes simultaneous RadFI on /data

beamfs RECOVERED 12/12 (4 nodes x 3 probs) reproducibly.
DIFFS=0 across all nodes, all probabilities, all 12 files matching
pre-attack hash. Robust under cluster-wide saturation (1M ppm = 100%).

### 3.3 bitrot : at-rest dd corruption

20 observations (5 FS x 4 cells). beamfs scheme=5 logs RS journal
entries on detection; non-FEC FSes silently propagate corruption to
read.

### 3.4 metadata : RadFI deterministic block targeting

20 observations. RadFI flips on block 0 (SB), 1 (bitmap), 2 (inode-
adjacent), and saturation x3.

A4 saturation pattern : non-FEC FSes typically saturate at iter=1
(the SB cannot be remounted after first corruption). beamfs scheme=5
absorbs the 3 iterations.

### 3.5 crash : virsh destroy mid-write recovery

5 observations. All 4 R/W filesystems (ext4, ext3, btrfs, beamfs)
remount cleanly with mount_rc=0 and stable_files_ok=5/5.
DMESG_JOURNAL_REPLAY logged in all 4 cases (kernel signal a recovery
event happened). DMESG_PANIC=0 across all FSes.

### 3.6 fsck : offline check

4 observations. ext4, ext3, btrfs return fsck_rc=0 (clean post-bench).
beamfs returns NOT_IMPLEMENTED (fsck.beamfs is Phase 2 mainline-prep).

---

## 4. Open questions (post-run analysis required)

- DMESG_RS_CORRECTED=0 on metadata observations for beamfs : possible
  page cache hit during remount, RadFI flip not reaching FEC decode
  path. To be diagnosed with strace + blktrace on remount path.
- A4 saturation observability : non-FEC FSes saturate at iter=1, but
  the bench currently emits a 3-iter run regardless. Documented as
  saturation_reached_no_remount observation; acceptable.

---

## 5. References

- Live runs : `~/git/yocto-beamfs/Documentation/runs/Tir-*-*/`
- Synthesis output per run : `synthesis.md` + `all-records.txt`
- Forensic tarballs : `Tir-analyse-multifs-full-*.tar.gz`
- Paper v3 working draft : `papers/2026-04-beamfs-v3-findings/`

End of empirical state snapshot.


## N=100 multifs (2026-05-10)

**Run reference :** `/tmp/N100-20260510-003122/`
**Raw data tarball :** `N100-raw-data.tar.gz`
**SHA-256 :** `f9f01a585c2871cab063ffbf2bbfc015777537918339a5d742b2137846e8a615`
**Total records :** 6000 (2 batches × 100 runs × 5 FS × 6 probs)
**Runtime :** ~17h45m (start 2026-05-10 00:31, end 18:21)
**FS_PANIC count :** 0
**Stack versions :**
  - kernel 7.0.3 (qemuarm64)
  - beamfs.ko v0.1.1 (devel commit 364e445)
  - emufi.ko v0.3.5 (main commit 9c2dbf4, dual-hook)
  - beamfs-bench v0.10.0 (main commit f1e347b, post cluster.rs propagation fix)

### Configuration

The run executed two sequential batches sharing identical fault
injection parameters but different `s_data_protection_scheme` values :

  - **Batch A** : `BEAMFS_SCHEME=inline` (scheme=2 UNIVERSAL_INLINE).
    FS list : beamfs / ext4 / btrfs / xfs / ext2.
  - **Batch B** : `BEAMFS_SCHEME=inode-universal` (scheme=5 INODE_UNIVERSAL).
    FS list : beamfs / f2fs / exfat / vfat / squashfs.

Per-cell parameters :
  - 6 probability levels : 100 / 1000 / 10000 / 100000 / 500000 / 1000000 ppm
  - `flip_locality = 0` (RANDOM, default)
  - `flip_width = 1` (default), `burst_symbols = 1` (default)
  - `target_block_range` calibrated to the beamfs file layout
    (sectors 2440..2960 typical, dynamic per-run via filefrag)

### Batch A results (scheme=2 UNIVERSAL_INLINE)

| FS | 100 ppm | 1k ppm | 10k ppm | 100k ppm | 500k ppm | 1M ppm |
|---|---|---|---|---|---|---|
| beamfs scheme=2 | 100% | 100% | 100% | 99% | 96% | **89%** + 60 UNC |
| ext4 | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| btrfs | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| xfs | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| ext2 | 100% | 100% | 100% | 90% | 49% | **0%** (silent) |

Per-cell flip activity (beamfs scheme=2 only) :

| Prob | FLIP_mean | RS_CORRECTED_mean | UNC_total | BITS_DIFF |
|---|---|---|---|---|
| 100 | 0.01 | 0.01 | 0 | 0 |
| 1k | 0.05 | 0.05 | 0 | 0 |
| 10k | 0.66 | 0.69 | 0 | 0 |
| 100k | 6.82 | 6.71 | 0 | 0 |
| 500k | 57.03 | 9.11 | 0 | 0 |
| 1M | 185.06 | 5.07 | **60** | 15431 |

### Batch B results (scheme=5 INODE_UNIVERSAL)

| FS | 100 ppm | 1k ppm | 10k ppm | 100k ppm | 500k ppm | 1M ppm |
|---|---|---|---|---|---|---|
| beamfs scheme=5 | 99% | 95% | 54% | **0%** | **0%** | **0%** |
| f2fs | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| exfat | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| vfat | 100% | 100% | 100% | 100% | 100% | 100% (no flip) |
| squashfs | 100% | 100% | 100% | 100% | 100% | 100% (RO) |

Per-cell flip activity (beamfs scheme=5 only) :

| Prob | FLIP_mean | RS_CORRECTED_mean | UNC_total | BITS_DIFF |
|---|---|---|---|---|
| 100 | 0.01 | **0.00** | 0 | 1 |
| 1k | 0.05 | **0.00** | 0 | 5 |
| 10k | 0.61 | **0.00** | 0 | 61 |
| 100k | 6.48 | **0.00** | 0 | 637 |
| 500k | 31.21 | **0.00** | 0 | 35901 |
| 1M | 63.06 | **0.00** | 0 | 55310 |

### Scientific observations

1. **Theorem v2.1 corroboration (scheme=2)**. The dose-response
   curve up to 500k ppm shows clean recovery : `RS_CORRECTED_mean`
   tracks `FLIP_mean` until ~9 errors, the per-subblock RS(255,239)
   capacity (`t = (n-k)/2 = 8`) is reached at 500k ppm, and 96%
   hash preservation is maintained.

2. **Theorem v2.2 corroboration (scheme=2)**. At 1M ppm with
   `flip_locality=RANDOM`, 60 `BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE`
   journal entries are emitted across 100 runs (60% of runs in
   saturation). The system correctly signals the saturation
   regime instead of producing silent corruption.

3. **Scheme=5 data block protection gap (empirical)**. Across
   600 cells of scheme=5, the cumulative `RS_CORRECTED` count is
   exactly **zero**. Cumulative `BITS_DIFF` reaches 92,005 bits.
   This is consistent with the source code analysis :
   `file.c::generic_file_read_iter` (the path for files >
   `BEAMFS_DATA_INLINE_BYTES = 3824 bytes`) does not invoke
   `beamfs_rs_decode_region` on the data block read path. The
   gap is documented in `Documentation/roadmap.md` Stage 4
   (universal data block protection, PENDING).

4. **ext2 silent corruption confirmed**. At 1M ppm, ext2
   produces 0/100 hash preservation with 0 RS_CORRECTED (no FEC)
   and BITS_DIFF=16375. This corroborates the paper v2 narrative
   on commodity FS silent corruption under EM stress.

### Limitations

The Batch A and Batch B results for **commodity FS other than
beamfs and ext2** are **invalid for resistance comparison**. The
`target_block_range` filter was calibrated to the beamfs file
layout (sectors corresponding to file blocks on the beamfs
device). The same range applied to ext4 / btrfs / xfs / f2fs /
exfat / vfat / squashfs targets device sectors that do not
correspond to the file on those FS. Consequently, FLIP_mean is
0.00 for all those FS, and their 100% hash preservation reflects
**absence of injection**, not resilience.

This limitation is structural to emufi v0.3.5 (one
`target_block_range` per attack window). Equitable multifs
comparison requires the emufi v0.4 multifs-capable injector
described in `~/git/emufi/Documentation/roadmap.md`.

### Reproducibility

Raw data preserved at `/tmp/N100-20260510-003122/` on
spartian (research deployment). The CSV `N100-records.csv`
(6001 lines) is the canonical extraction. Re-extraction from
the tarball is documented in `Documentation/HOW-TO-beamfs-globally.md`.
