# Multi-FS head-to-head bench under RadFI live injection (2026-04-30)

## Context

Empirical validation of BEAMFS v2 INLINE Reed-Solomon recovery scheme
(DOI 10.5281/zenodo.19886192) against four mainstream filesystems
(ext4, ext3, btrfs, squashfs) under RadFI v0.1.3 live fault injection
(DOI 10.5281/zenodo.19885777).

This bench complements the BEAMFS v2 paper by establishing a comparative
baseline: rather than asserting BEAMFS resilience in isolation, we
measure where BEAMFS sits relative to the existing FS landscape under
the same simulated SEU pressure.

## Methodology

### Hardware setup (physical USB passthrough)

5 USB physical disks attached to a single VM (beamfs-master) via libvirt
passthrough block-mode (`type='block'`, `cache='none'`, `io='threads'`).
The host page cache is bypassed; bio-level SEU injection by RadFI
v0.1.3 reaches the physical media without intermediate caching.

| FS       | Device | Hardware                          |
|----------|--------|-----------------------------------|
| ext4     | vdc    | Kingston DataTraveler 3.0 (57 GB) |
| ext3     | vdd    | Kingston DataTraveler 3.0 (57 GB) |
| btrfs    | vde    | Kingston DataTraveler 3.0 (57 GB) |
| squashfs | vdf    | Kingston DataTraveler 3.0 (14 GB) |
| BEAMFS   | vdg    | SanDisk Cruzer (3.8 GB)           |

Per-FS partition: 2 GB GPT, single primary partition. Test data
identical across FS:
- 3 directories (`dir-A`, `dir-B`, `dir-C`)
- Per directory: 3 files of 3 KB random data + `HASHES.sha256`
- Total per partition: 9 data files + 3 HASHES files = 12 files

### Why 3 KB per file (not 300 KB)

BEAMFS v2 INLINE scope (`BEAMFS_DATA_INLINE_BYTES = 3824`) caps single-
file size at 3824 bytes (single-block scope, `file_inline.c`
`beamfs_inline_write_begin` returns -EFBIG above). To maintain methodo-
logical fairness across all 5 FS, all files were sized to 3 KB.

The multi-block extension is in roadmap (`roadmap.md` section
"mkfs.beamfs userspace evolution") and is required before larger
workloads (MB-scale files) can be benched scientifically.

### Attack model

For each (FS, prob) pair:
1. RadFI v0.1.3 armed with `target_dev = MKDEV(maj, min)` of the FS
   partition, `target_block = 0` (wildcard, full partition scope) — block-
   precise targeting was not used in this run; planned for the next
   iteration when `filefrag` is added to the image
2. `enabled=1, hook_blk=1` activated, then trigger I/O on
   `dir-B/file-B2.bin`
3. RadFI disarmed, drop_caches, umount, remount, recompute hashes
   of all 12 files
4. Verdict classification:
   - **RECOVERED**: all 12 hashes match pre-attack
   - **CORRUPTED_DATA**: 1-3 file hashes diverge
   - **CORRUPTED_HEAVY**: 4+ file hashes diverge
   - **FS_PANIC**: remount fails OR I/O errors on read

Three probabilities tested per FS: 1000 ppm (low), 100000 ppm (10%),
1000000 ppm (saturation 100%).

## Results

| FS       | prob 1000 ppm        | prob 100000 ppm        | prob 1000000 ppm        |
|----------|----------------------|------------------------|-------------------------|
| ext4     | RECOVERED (0 flips)  | RECOVERED (0 flips)    | **FS_PANIC** (30 flips, remount failed) |
| ext3     | RECOVERED (0 flips)  | RECOVERED (11 flips)   | RECOVERED (101 flips, see caveat) |
| btrfs    | RECOVERED (0 flips)  | RECOVERED (2 flips)    | **DETECTED_REJECTED** (15 flips, I/O error on read) |
| squashfs | RECOVERED (0 flips)  | RECOVERED (3 flips)    | RECOVERED (13 flips, see caveat) |
| BEAMFS   | RECOVERED (0 flips)  | RECOVERED (0 flips)    | **RECOVERED** (11 flips, RS-decode active) |

## Observations

### BEAMFS — primary observation

At 100% injection probability, 11 bit flips were injected on the BEAMFS
data path. After umount/remount (forcing disk re-read), all 12 file
hashes match pre-attack. This is consistent with the Reed-Solomon
INLINE-block recovery model published in v2.

### ext4 — FS_PANIC at saturation

At 100% probability with 30 bit flips, ext4 cannot remount the
partition. This is a journal corruption pathology: ext4 with
`data=ordered` (default) maintains data write ordering but does not
protect block content. When journal blocks are corrupted, replay fails
and the FS is unrecoverable without manual `e2fsck` intervention.

### btrfs — DETECTED_REJECTED at saturation

At 100% probability, btrfs raises `Input/output error` on all attempted
reads. This is btrfs functioning **as designed**: the per-extent CRC32
detects every bit flip and refuses to serve corrupted blocks. The
"FS_PANIC" classification in the test harness was overly strong; this
is more accurately **DETECTED_REJECTED** — the FS detects corruption
and chooses to fail rather than serve incorrect data, the conservative
choice. A correctly designed application sees `EIO` and can react.
BEAMFS in contrast attempts RS-decode reconstruction transparently
before raising errors — a different design tradeoff.

### ext3 — 101 flips RECOVERED (caveat)

The result that ext3 with 101 injected bit flips remounts cleanly and
returns matching hashes appears too good. Hypothesis: in ext3
`data=ordered`, file data is written through the journal first; the
journal blocks may have been replayed cleanly, and corrupted blocks on
the data area may not have been read after remount because they were
ordered into clean blocks elsewhere.

This needs investigation. The current verdict is suspicious and should
not be cited as evidence of ext3 SEU resilience without further
analysis.

### squashfs — RECOVERED with possibly insufficient cache flushing

squashfs at 100% probability shows 13 flips intercepted. The fix added
in this version is `umount + drop_caches + remount` before the trigger
read, which forces real bio reads from disk.

However, squashfs uses xz compression by default. At 100% prob, 13 bit
flips in compressed metadata blocks should produce decompression
failure or read-back of incorrect data. The RECOVERED verdict suggests
either (a) the flips landed exclusively on compressed-padding bytes
that decompression tolerated, or (b) the test methodology still has
caching artifacts to investigate.

This is the second result that needs deeper analysis.

## Caveats and limitations (honesty)

1. **3 KB file size**: imposed by BEAMFS v2 INLINE scope. Mainstream
   FS comparison with realistic workloads (1+ MB files) blocked until
   `extent-multiblock` scheme implementation.

2. **target_block = 0 (wildcard)**: bit flips landed randomly across
   metadata + data + journal. For block-precise injection, the next
   bench iteration must include `filefrag` in the image and pre-locate
   `dir-B/file-B2.bin`'s data block.

3. **Single-trial per (FS, prob)**: the 100% prob results in particular
   need 10+ trials per FS to observe variance.

4. **squashfs and ext3 RECOVERED at 100%**: under-investigated.

5. **BEAMFS RECOVERED at 100%**: consistent with v2 paper but the
   workload is small (3 KB single-block INLINE).

## Reproducibility

The bench is fully reproducible from the yocto-beamfs lab repository:

- Lab harness: `bin/Tir-multifs.sh` + `bin/Tir-analyse-multifs.sh`
- VM image recipe: `recipes-core/images/hpc-arm64-research-beamfs.bb`
  (kernel-modules + btrfs-tools + squashfs-tools)
- Kernel: `linux-mainline 7.0.3` (kernel.org stable, dated 2026-04-30,
  SRCREV `9d0263b7446c985ffec3eb48a17104dfe4bc1115`)
  + multifs.cfg fragment
- RadFI v0.1.3 (with `target_block` filter on bio hook)

Run artifact: `Documentation/runs/Tir-analyse-multifs-20260430-141008.tar.gz`

## Next steps

1. Implement BEAMFS scheme=extent-multiblock (11-15h, see roadmap)
2. Add `filefrag` to image for block-precise target_block
3. Re-run multi-FS bench with realistic file sizes (1 MB, 10 MB, 100 MB)
4. Add 10-trial repetitions per (FS, prob) for variance analysis
5. Investigate ext3 / squashfs anomalies above
6. Prepare BEAMFS v3 paper draft incorporating multi-FS bench results

