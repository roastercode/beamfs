# Multi-FS head-to-head bench under RadFI live injection (2026-04-30)

## Context

Empirical validation of beamfs v2 INLINE Reed-Solomon recovery scheme
(DOI 10.5281/zenodo.19886192) against four mainstream filesystems
(ext4, ext3, btrfs, squashfs) under RadFI v0.1.3 live fault injection
(DOI 10.5281/zenodo.19885777).

This bench complements the beamfs v2 paper by establishing a comparative
baseline: rather than asserting beamfs resilience in isolation, we
measure where beamfs sits relative to the existing FS landscape under
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
| beamfs   | vdg    | SanDisk Cruzer (3.8 GB)           |

Per-FS partition: 2 GB GPT, single primary partition. Test data
identical across FS:
- 3 directories (`dir-A`, `dir-B`, `dir-C`)
- Per directory: 3 files of 3 KB random data + `HASHES.sha256`
- Total per partition: 9 data files + 3 HASHES files = 12 files

### Why 3 KB per file (not 300 KB)

beamfs v2 INLINE scope (`BEAMFS_DATA_INLINE_BYTES = 3824`) caps single-
file size at 3824 bytes (single-block scope, `file_inline.c`
`beamfs_inline_write_begin` returns -EFBIG above). To maintain methodo-
logical fairness across all 5 FS, all files were sized to 3 KB.

The multi-block extension is in roadmap (`roadmap.md` section
"mkfs.beamfs userspace evolution") and is required before larger
workloads (MB-scale files) can be benched scientifically.

### Attack model

For each (FS, prob) pair:
1. RadFI v0.1.3 armed with `target_dev = MKDEV(maj, min)` of the FS
   partition, `target_block = 0` (wildcard, full partition scope) - block-
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
| beamfs   | RECOVERED (0 flips)  | RECOVERED (0 flips)    | **RECOVERED** (11 flips, RS-decode active) |

## Observations

### beamfs - primary observation

At 100% injection probability, 11 bit flips were injected on the beamfs
data path. After umount/remount (forcing disk re-read), all 12 file
hashes match pre-attack. This is consistent with the Reed-Solomon
INLINE-block recovery model published in v2.

### ext4 - FS_PANIC at saturation

At 100% probability with 30 bit flips, ext4 cannot remount the
partition. This is a journal corruption pathology: ext4 with
`data=ordered` (default) maintains data write ordering but does not
protect block content. When journal blocks are corrupted, replay fails
and the FS is unrecoverable without manual `e2fsck` intervention.

### btrfs - DETECTED_REJECTED at saturation

At 100% probability, btrfs raises `Input/output error` on all attempted
reads. This is btrfs functioning **as designed**: the per-extent CRC32
detects every bit flip and refuses to serve corrupted blocks. The
"FS_PANIC" classification in the test harness was overly strong; this
is more accurately **DETECTED_REJECTED** - the FS detects corruption
and chooses to fail rather than serve incorrect data, the conservative
choice. A correctly designed application sees `EIO` and can react.
beamfs in contrast attempts RS-decode reconstruction transparently
before raising errors - a different design tradeoff.

### ext3 - 101 flips RECOVERED (caveat)

The result that ext3 with 101 injected bit flips remounts cleanly and
returns matching hashes appears too good. Hypothesis: in ext3
`data=ordered`, file data is written through the journal first; the
journal blocks may have been replayed cleanly, and corrupted blocks on
the data area may not have been read after remount because they were
ordered into clean blocks elsewhere.

This needs investigation. The current verdict is suspicious and should
not be cited as evidence of ext3 SEU resilience without further
analysis.

### squashfs - RECOVERED with possibly insufficient cache flushing

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

## Capabilities matrix vs tests actually run

The benchmark above tested ONE specific resilience axis: bit-flip
corruption of user data injected at the bio layer during write and read.
Each filesystem has additional resilience mechanisms that this benchmark
did NOT exercise. This section makes that scope explicit so the result
is not over-generalized.

### Native capabilities claimed by each FS

| Capability                       | ext4               | ext3               | btrfs               | squashfs           | beamfs v2          |
|----------------------------------|--------------------|--------------------|---------------------|--------------------|--------------------|
| Journal metadata                 | yes (jbd2)         | yes (jbd)          | no (COW instead)    | n/a (RO)           | no (planned v4)    |
| Journal data (data=journal)      | yes (option)       | yes (option)       | n/a                 | n/a                | no                 |
| Crash consistency                | journal replay     | journal replay     | COW + redundant SBs | n/a                | partial (sync)     |
| Checksum metadata                | yes (metadata_csum)| no                 | yes (CRC32 all)     | yes (MD5/CRC32)    | indirect (RS)      |
| Checksum data                    | no                 | no                 | yes (CRC32 extent)  | yes (compr header) | indirect (RS)      |
| FEC / error correction on data   | no                 | no                 | no (detect only)    | no (detect only)   | yes RS(255,239)    |
| Auto-repair via redundancy       | no                 | no                 | yes if RAID1+/DUP   | no                 | yes via RS-decode  |
| fsck offline                     | yes (e2fsck)       | yes (e2fsck)       | yes (btrfs check)   | no (RO immutable)  | no (planned v3)    |
| Online scrub                     | no                 | no                 | yes (btrfs scrub)   | no                 | no                 |
| Snapshots                        | no                 | no                 | yes (subvolumes)    | no                 | no                 |
| Compression FS                   | no                 | no                 | yes (zstd/lzo)      | yes (xz/zstd/lzo)  | no                 |
| Internal RAID                    | no                 | no                 | yes (RAID0/1/10/5/6)| no                 | no (planned)       |
| Bit-flip data tolerance          | no                 | no                 | detect (EIO)        | partial            | correct (transparent) |
| dm-verity compatibility          | yes                | yes                | yes                 | yes                | yes (in theory)    |

### What this benchmark actually exercised

| Test path                          | Mechanism triggered     | FS that claim it     | FS that were stressed   |
|------------------------------------|-------------------------|----------------------|-------------------------|
| Bit-flip on bio I/O write          | Journal data + replay   | ext4, ext3           | ext4, ext3              |
| Bit-flip on bio I/O read           | Detection via checksum  | btrfs, squashfs      | btrfs, squashfs partial |
| Bit-flip on bio I/O read           | Correction via FEC      | beamfs only          | beamfs                  |
| Saturation 100% prob               | Recovery limit          | all                  | ext4 panic, btrfs detect+reject, beamfs recover |

### Per-FS: what was tested vs what is claimed

| FS       | Tested by this bench                | Native capabilities NOT tested here                                                |
|----------|-------------------------------------|------------------------------------------------------------------------------------|
| ext4     | bit-flip data write+read (3 KB)     | metadata_csum, journal-targeted corruption, e2fsck recovery, barriers, fsync ordering |
| ext3     | same                                | journal replay isolated, fsck recovery, ordered/writeback/journal modes            |
| btrfs    | CRC32 detection per extent          | scrub auto-repair (DUP/RAID1), snapshot rollback, send/receive, compression integrity, RAID modes |
| squashfs | Compression-header detection (3 KB) | dm-verity stack, root signing, multi-block compressed integrity, large-file streaming |
| beamfs   | RS-decode INLINE 3 KB single-block  | extent-multiblock RS (not implemented), metadata RS isolated, crash consistency, dirty-block bitmap repair |

### Honest scope of the result

Under the specific attack tested (single-bit-flip on user data at
`submit_bio_noacct`, target_block=0 wildcard, file size 3 KB INLINE),
beamfs is the only filesystem in the comparison that corrects
transparently and survives 100% probability injection. ext4 panics
at saturation, btrfs detects and rejects (returns `-EIO` to userspace),
ext3 and squashfs RECOVERED verdicts at the highest probability are
suspect (see Caveats below).

This validates the beamfs RS(255,239) inline correction layer for the
tested workload. It does NOT establish beamfs superiority on any of
the other axes listed in the capabilities matrix. A fair filesystem
comparison for a kernel mailing-list submission needs at least:

1. Metadata-targeted attack (Test A in the next-steps roadmap):
   superblock, inode bitmap, journal range; verdicts where ext4
   without `metadata_csum` should fail, btrfs should detect, beamfs
   should correct via the inode RS protection.
2. Crash consistency (Test B): `virsh destroy` mid-write, restart,
   remount, integrity check; ext4/ext3 journal replay should win,
   btrfs COW atomic should win, beamfs losing the in-flight write
   is acceptable but must be measured.
3. Bit-rot offline (Test C): write data, unmount, `dd` random bytes
   directly on partition, remount, read; only btrfs (CRC) and beamfs
   (RS) should detect or correct, ext4/ext3 are expected to silently
   serve corrupted data.
4. fsck recovery post-FS_PANIC (Test D): reproduce ext4 FS_PANIC at
   100% prob, run `e2fsck -y`, compare hashes; measure the actual
   recovery rate of a corrupted journal.

These four tests are the scope of the next iteration of the harness
(Tir-multifs-{metadata,crash,bitrot,fsck}.sh) and will be added to
the bench v2 report.

## Caveats and limitations (honesty)

1. **3 KB file size**: imposed by beamfs v2 INLINE scope. Mainstream
   FS comparison with realistic workloads (1+ MB files) blocked until
   `extent-multiblock` scheme implementation.

2. **target_block = 0 (wildcard)**: bit flips landed randomly across
   metadata + data + journal. For block-precise injection, the next
   bench iteration must include `filefrag` in the image and pre-locate
   `dir-B/file-B2.bin`'s data block.

3. **Single-trial per (FS, prob)**: the 100% prob results in particular
   need 10+ trials per FS to observe variance.

4. **squashfs and ext3 RECOVERED at 100%**: under-investigated.

5. **beamfs RECOVERED at 100%**: consistent with v2 paper but the
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

1. Implement beamfs scheme=extent-multiblock (11-15h, see roadmap)
2. Add `filefrag` to image for block-precise target_block
3. Re-run multi-FS bench with realistic file sizes (1 MB, 10 MB, 100 MB)
4. Add 10-trial repetitions per (FS, prob) for variance analysis
5. Investigate ext3 / squashfs anomalies above
6. Prepare beamfs v3 paper draft incorporating multi-FS bench results

