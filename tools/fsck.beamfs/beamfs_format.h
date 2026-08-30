/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * beamfs -- on-disk format
 *
 * Author: Aurelien Desbrieres <aurelien@hackers.camp>
 *
 * Every structure and constant that describes bytes on the medium,
 * and nothing else. No kernel types, so mkfs.beamfs and fsck.beamfs
 * include this file directly rather than keeping their own copies.
 *
 * They used to. Three defects in a single day came from the copies
 * drifting: fsck rejected the reserved inodes' pointers, mkfs marked
 * the canary block with an index it computed differently, and fsck's
 * superblock struct lagged the kernel's by sixteen bytes so every
 * offset past them was wrong. The format is one thing; it should be
 * written down once.
 *
 * ext4 and xfs split their headers the same way, for the same reason.
 */

#ifndef _BEAMFS_FORMAT_H
#define _BEAMFS_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
/*
 * Userspace build: the __leNN types are kernel spellings of plain
 * fixed-width integers. beamfs stores little-endian on disk and every
 * access goes through le*_to_cpu, so the userspace tools -- which run
 * on little-endian hosts and on the same little-endian targets --
 * read them directly.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <linux/types.h>   /* __le16/__le32/__le64/__uNN, as the kernel spells them */

typedef uint64_t u64;
typedef uint32_t u32;

/*
 * beamfs stores little-endian. The tools run on little-endian hosts
 * and targets, so the conversions are identities here; they stay
 * written out so the access sites read the same in both builds.
 */
#define le16_to_cpu(x) ((uint16_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))
#define le64_to_cpu(x) ((uint64_t)(x))
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))

#ifndef __packed
#define __packed __attribute__((packed))
#endif
#endif


/* inode_state_read_once returns inode_state_flags in kernel 7.0 */
#define beamfs_inode_is_new(inode) \
	(inode_state_read_once(inode) & I_NEW)

/* Magic number: 'FTRF' */
#define BEAMFS_MAGIC         0x4245414D

/* Block size: 4096 bytes */
#define BEAMFS_BLOCK_SIZE    4096
#define BEAMFS_BLOCK_SHIFT   12

/* RS FEC: 16 parity bytes per 239-byte subblock (RS(255,239)) */
#define BEAMFS_RS_PARITY     16
#define BEAMFS_INODE_RS_DATA offsetof(struct beamfs_inode, i_reserved)  /* 172 bytes */
#define BEAMFS_INODE_RS_PAR  16  /* parity bytes stored in i_reserved[0..15] */
#define BEAMFS_SUBBLOCK_DATA 239
#define BEAMFS_SUBBLOCK_TOTAL (BEAMFS_SUBBLOCK_DATA + BEAMFS_RS_PARITY)

/* On-disk bitmap block layout (RS FEC protected) */
#define BEAMFS_BITMAP_SUBBLOCKS  16   /* subblocks per bitmap block */
#define BEAMFS_BITMAP_DATA_BYTES (BEAMFS_BITMAP_SUBBLOCKS * BEAMFS_SUBBLOCK_DATA) /* 3824 */
/* Bits of free-data-block bitmap held by a single on-disk bitmap block. */
#define BEAMFS_BITS_PER_BITMAP_BLOCK (BEAMFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/*
 * Encoding of s_flags (on-disk).
 *
 * Bits 0..15 = bitmap_blocks_count (zero -> legacy 1, max 65535).
 * Bits 16..31 = reserved (must be zero).
 *
 * s_flags is __le32, inside the CRC32 coverage (region A 0..63).
 * Volumes formatted by pre-multi-bitmap mkfs have s_flags=0, which
 * matches the legacy single-block layout via the fallback below.
 */
#define BEAMFS_SB_FLAGS_BITMAP_BLOCKS_MASK  0x0000FFFFu

/*
 * On-disk data block layout under BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE.
 *
 * Each user data block (4096 bytes on disk) holds 16 RS(255,239) shortened
 * subblocks, identical layout to bitmap blocks. Per subblock: 239 data bytes
 * followed by 16 parity bytes (sb total = 255). The 16 subblocks together
 * occupy 16 * 255 = 4080 bytes; the last 16 bytes of the block are zero pad.
 *
 * Logical capacity per disk block: 16 * 239 = 3824 bytes.
 * Correction capacity per disk block: 16 * (16/2) = 128 bytes corrigeable
 * (8 byte symbols per subblock, 16 subblocks).
 *
 * file_size_logical = N user-visible bytes
 * disk_blocks_used  = ceil(N / BEAMFS_DATA_INLINE_BYTES)
 *
 * Selected and validated against RadFI v0.1.0 (1-bit flip per submit_bio).
 */
#define BEAMFS_DATA_INLINE_SUBBLOCKS  16
#define BEAMFS_DATA_INLINE_BYTES      (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_SUBBLOCK_DATA)  /* 3824 */
#define BEAMFS_DATA_INLINE_TOTAL      (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_SUBBLOCK_TOTAL) /* 4080 */
#define BEAMFS_DATA_INLINE_PAD        (BEAMFS_BLOCK_SIZE - BEAMFS_DATA_INLINE_TOTAL)         /* 16  */

/*
 * DATA_CSUM descriptor layout inside the 16-byte block tail pad
 * [BEAMFS_DATA_INLINE_TOTAL .. BEAMFS_BLOCK_SIZE). Not part of any RS
 * codeword. See format-v6.md section 3.1. Only 8 bytes are used; the
 * remaining 8 stay zero-filled and reserved.
 */
#define BEAMFS_DATA_CSUM_TYPE_OFF     (BEAMFS_DATA_INLINE_TOTAL + 0)  /* 4080 u8      */
#define BEAMFS_DATA_CSUM_VALUE_OFF    (BEAMFS_DATA_INLINE_TOTAL + 4)  /* 4084 __le32  */
#define BEAMFS_DATA_CSUM_DESC_BYTES   8                               /* type+rsvd+csum */
/*
 * DATA_SELFID: block self-identification, in the second half of the tail
 * pad. DATA_CSUM proves a block's contents are intact; it cannot prove the
 * block is the right one. A corrupted block pointer that still lands inside
 * the data area addresses a different, perfectly valid block, whose own
 * descriptor verifies. Measured 2026-08-19 at a 128-flip budget: an inode's
 * direct pointer went from physical 471 to 503, the read returned that
 * block's contents (15245 wrong bits) with cat exiting 0 and no kernel
 * signal, because every integrity check passed on a block that simply was
 * not the one asked for. The existing bounds check only rejects pointers
 * outside [s_data_start, s_data_start + s_nblocks).
 *
 * The field holds beamfs_data_selfid(ino, iblock), so a read that lands on
 * the wrong block fails closed. A 64-bit digest rather than the raw pair
 * keeps the field within the 8 reserved bytes without capping inode numbers
 * or file size, which matters for the datacenter workloads this filesystem
 * targets as much as for embedded ones.
 */
#define BEAMFS_DATA_SELFID_OFF        (BEAMFS_DATA_INLINE_TOTAL + 8)  /* 4088 __le64  */
#define BEAMFS_DATA_SELFID_BYTES      8

/*
 * Conformance fixture (canary block) -- v4 INLINE only.
 *
 * Under BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE, mkfs.beamfs writes a
 * deterministic canary block immediately after the root directory
 * block. The canary block is RS-encoded exactly like a regular INLINE
 * data block (16 interleaved subblocks of 239+16 bytes plus 16 bytes
 * pad) and contains a fixed 64-byte ASCII header followed by a
 * deterministic 3760-byte payload. The byte layout is reproducible
 * across mkfs invocations; its sha256 is published in
 * Documentation/format-v4.md section 13.
 *
 * Purpose: validate the on-disk RS encode chain end-to-end without
 * relying on user-written content. Stage 1 (Palier 1) writes the
 * block and exposes it on-disk only. Stage 2 (Palier 2) will add an
 * immutable VFS alias inode for kernel read-path validation.
 */
#define BEAMFS_CANARY_HEADER_LEN      64
#define BEAMFS_CANARY_PAYLOAD_LEN     (BEAMFS_DATA_INLINE_BYTES - BEAMFS_CANARY_HEADER_LEN) /* 3760 */
#define BEAMFS_CANARY_USER_BYTES      BEAMFS_DATA_INLINE_BYTES                              /* 3824 */
#define BEAMFS_CANARY_HEADER_STR      "BEAMFS-CANARY-v4 RS(255,239)x16 SHA256-fixed\n"

/*
 * Reserved inode numbers.
 *
 * Inodes [0 .. BEAMFS_FIRST_USER_INO) are reserved for FS-internal
 * fixtures (root directory, canary block alias). User-visible inode
 * allocation starts at BEAMFS_FIRST_USER_INO.
 *
 * Operations that would mutate or free reserved inodes are rejected
 * by defense-in-depth: VFS layer (S_IMMUTABLE) and allocator layer
 * (range check in beamfs_free_block / beamfs_free_inode_num).
 */
#define BEAMFS_RESERVED_INO_ROOT      1
#define BEAMFS_RESERVED_INO_CANARY    2
#define BEAMFS_FIRST_USER_INO         3

static inline bool beamfs_ino_is_reserved(u64 ino)
{
	return ino < BEAMFS_FIRST_USER_INO;
}

/*
 * Translation helpers for BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE.
 *
 * The user-visible file_offset (in bytes) maps to a logical block index
 * iblock_logical = file_offset / BEAMFS_DATA_INLINE_BYTES (= /3824).
 * Each logical block backs exactly one physical disk block of
 * BEAMFS_BLOCK_SIZE (4096) bytes, holding 3824 user bytes interleaved
 * with 16*16=256 RS parity bytes plus 16 bytes of pad.
 *
 * For schemes other than UNIVERSAL_INLINE, translation is the legacy
 * 1:1 with BEAMFS_BLOCK_SIZE granularity; callers must guard on
 * sbi->s_scheme before invoking these helpers.
 */
static inline u64 beamfs_inline_logical_to_iblock(u64 file_offset)
{
	return file_offset / BEAMFS_DATA_INLINE_BYTES;
}

static inline u32 beamfs_inline_offset_in_logical(u64 file_offset)
{
	return (u32)(file_offset % BEAMFS_DATA_INLINE_BYTES);
}

static inline u64 beamfs_inline_size_to_blocks(u64 size)
{
	return (size + BEAMFS_DATA_INLINE_BYTES - 1) / BEAMFS_DATA_INLINE_BYTES;
}



/*
 * Superblock RS layout (stage 3 item 4, v4 format).
 *
 * The superblock CRC32 covers two non-contiguous regions:
 *   region A  [0, 64)        = 64 bytes (header + counters + version + flags)
 *   region B  [68, 2713)     = 2645 bytes (uuid + label + RS journal +
 *                              bitmap_blk + features + protection scheme)
 *   total                   = 2709 logical bytes
 *   excluded: s_crc32 itself in [64, 68).
 *
 * RS protection covers exactly the same 2709 logical bytes. The two
 * regions are serialized into a contiguous 2743-byte staging buffer
 * (34 bytes of zero pad to round up to 13 shortened subblocks of 211
 * data bytes each). RS(255,239) shortened with data_len=211 is the
 * standard lib/reed_solomon usage; padding is implicit.
 *
 * Each of the 13 subblocks tolerates up to 8 symbol errors
 * independently (RS_PARITY/2 = 16/2 = 8). Total correction capacity
 * across the superblock: 104 symbol errors when distributed across
 * subblocks. MIL-STD-882E favourable: 13 independent failure-
 * correctable regions, up from 8 in v3.
 *
 * The 208 bytes of parity (13 * 16) live at sb->s_pad[1175..1382],
 * which corresponds to disk offset 2713 + 1175 = 3888 -- the last
 * 208 bytes of the 4096-byte superblock. This trailing position is
 * stable against future format evolution: new fields go into s_pad
 * before the parity zone, and BEAMFS_SB_RS_S_PAD_INDEX is computed
 * from offsetof(s_pad) so the layout updates atomically.
 *
 * Growth from v3 (1685 cov / 8 subblocks / 128 parity / s_pad[2407])
 * to v4 (2709 cov / 13 subblocks / 208 parity / s_pad[1383]) accounts
 * for the 1024-byte enlargement of s_rs_journal[] driven by the
 * 24 -> 40 byte expansion of struct beamfs_rs_event (item 4).
 *
 * v5 adds s_ind_parity_blk and s_ind_parity_len, 16 bytes, so coverage
 * grows to 2725 and the shortened data_len drops 211 -> 210. Subblock
 * count, parity size and parity offset are unchanged, so correction
 * capacity stays at 8 symbols per subblock and 104 across the block.
 * The BUILD_BUG_ON in beamfs_sb_to_rs_staging catches exactly this
 * class of change, which is why it is there.
 */
#define BEAMFS_SB_RS_COVERAGE_BYTES  2725   /* logical bytes (CRC32 range) */
#define BEAMFS_SB_RS_STAGING_BYTES   2730   /* 13 * BEAMFS_SB_RS_DATA_LEN   */
#define BEAMFS_SB_RS_DATA_LEN        210    /* per shortened subblock      */
#define BEAMFS_SB_RS_SUBBLOCKS       13     /* total subblocks             */
#define BEAMFS_SB_RS_PARITY_BYTES    208    /* 13 * BEAMFS_RS_PARITY        */
#define BEAMFS_SB_RS_PARITY_OFFSET   3888   /* end - parity bytes          */
#define BEAMFS_SB_RS_S_PAD_INDEX     (BEAMFS_SB_RS_PARITY_OFFSET - 2729)
					  /* index in s_pad[]: 1175     */
#define BEAMFS_BITMAP_MAX_BLOCKS (BEAMFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/* Filesystem limits */
#define BEAMFS_MAX_FILENAME  255
#define BEAMFS_DIRECT_BLOCKS 12
#define BEAMFS_INDIRECT_BLOCKS 1
#define BEAMFS_DINDIRECT_BLOCKS 1

/*
 * BEAMFS_INDIRECT_PTRS: number of block pointers per indirect block.
 * Each pointer is a u64 (8 bytes), so 4096 / 8 = 512 entries.
 * Single indirect capacity: 512 blocks = 2 MiB.
 *
 * Shared between the legacy iomap path (file.c, scheme=5) and the
 * v2 INLINE block-mapping helpers (file_inline.c, scheme=2).
 */
#define BEAMFS_INDIRECT_PTRS (BEAMFS_BLOCK_SIZE / sizeof(__le64))

/*
 * Multi-level indirect capacity (v5.x extension, post-RFC v5.0 baseline).
 *
 * BEAMFS_DINDIRECT_PTRS: total user blocks reachable via the double
 *   indirect pointer = 512 single-indirect blocks x 512 ptrs each.
 * BEAMFS_TINDIRECT_PTRS: total user blocks reachable via the triple
 *   indirect pointer = 512 dindirect blocks x 512 single-indirect each
 *                      x 512 ptrs each.
 *
 * The i_dindirect and i_tindirect on-disk fields are already declared
 * in struct beamfs_inode and reserved in mkfs since v5.0; the kernel
 * implementation that walks these levels is introduced in v0.1.x
 * without a format bump.
 *
 * Maximum mapped iblock per indirection level:
 *   direct      :                                  BEAMFS_DIRECT_BLOCKS (12)
 *   + indirect  :                              524 (12 + 512)
 *   + dindirect :                          262 668 (524 + 512^2)
 *   + tindirect :                      134 480 396 (262 668 + 512^3)
 *
 * Logical user capacity per file (user bytes per block = 3824 under
 * scheme=2 UNIVERSAL_INLINE with 16 x RS(255,239) sub-blocks):
 *   direct      :    45 888 B  (~45 KiB)
 *   + indirect  : 2 003 776 B  (~1.91 MiB)
 *   + dindirect : ~ 956 MiB    (~1 GiB)
 *   + tindirect : ~ 478 GiB    (~512 GiB) -- current ceiling
 */
#define BEAMFS_DINDIRECT_PTRS  (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS)
#define BEAMFS_TINDIRECT_PTRS  (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS)

#define BEAMFS_MAX_IBLOCK_DIRECT     ((u64)BEAMFS_DIRECT_BLOCKS)
#define BEAMFS_MAX_IBLOCK_INDIRECT   (BEAMFS_MAX_IBLOCK_DIRECT   + (u64)BEAMFS_INDIRECT_PTRS)
#define BEAMFS_MAX_IBLOCK_DINDIRECT  (BEAMFS_MAX_IBLOCK_INDIRECT + (u64)BEAMFS_DINDIRECT_PTRS)
#define BEAMFS_MAX_IBLOCK_TINDIRECT  (BEAMFS_MAX_IBLOCK_DINDIRECT + (u64)BEAMFS_TINDIRECT_PTRS)

/*
 * Electromagnetic Resilience Journal entry -- 40 bytes (v4 format).
 *
 * Records each RS FEC correction event persistently in the superblock.
 * 64 entries give operators a map of physical degradation over time.
 * No existing Linux filesystem provides this at the block layer.
 *
 * Stage 3 item 4 introduces the per-event Shannon entropy estimate as
 * the forensic discriminator between Family A (Poisson background SEU)
 * and Family B (correlated burst). See Documentation/threat-model.md
 * section 6.4 and Documentation/format-v4.md for the algorithm.
 *
 * Layout invariants enforced at compile time:
 *   - 40 bytes, packed, no implicit padding (BUILD_BUG_ON in super.c)
 *   - re_reserved and re_pad MUST be zero on write; non-zero values
 *     act as structural sentinels and produce CRC mismatch on read
 *   - re_crc32 covers bytes [0..32), i.e. all fields except itself
 *     and the trailing alignment pad
 */
struct beamfs_rs_event {
	__le64  re_block_no;          /*  0..7   corrected block number     */
	__le64  re_timestamp;         /*  8..15  ktime_get_ns() at recovery */
	__le32  re_symbol_count;      /* 16..19  symbols corrected           */
	__le32  re_entropy_q16_16;    /* 20..23  Shannon H, Q16.16, [0,3*65536) */
	__le32  re_flags;             /* 24..27  see BEAMFS_RS_EVENT_FLAG_*  */
	__le32  re_reserved;          /* 28..31  zero, structural sentinel   */
	__le32  re_crc32;             /* 32..35  CRC32 over bytes [0..32)    */
	__le32  re_pad;               /* 36..39  zero, alignment + sentinel  */
} __packed;                       /* 40 bytes */

#define BEAMFS_RS_JOURNAL_SIZE  64   /* entries in the EM resilience journal   */

/*
 * Flags for beamfs_rs_event::re_flags. See Documentation/format-v4.md
 * section 6.5 for the normative contract.
 *
 * ENTROPY_VALID -- the re_entropy_q16_16 field carries a meaningful
 *                  Shannon estimate. Cleared by zero-init (mkfs);
 *                  set by beamfs_log_rs_event() when entropy is computed
 *                  from the position list returned by RS decode.
 *
 * UNCORRECTABLE -- the codeword exceeded the RS correction radius
 *                  (more than BEAMFS_RS_PARITY/2 = 8 symbols in error
 *                  within a single subblock). Data could not be
 *                  recovered; the read returned -EIO. When this flag
 *                  is set, re_symbol_count MUST be zero and
 *                  ENTROPY_VALID MUST be cleared. The flag is
 *                  orthogonal to Family A / Family B classification
 *                  (TM section 2): an uncorrectable event may originate
 *                  from either family; discrimination is performed
 *                  post-process by clustering analysis on re_block_no
 *                  and re_timestamp, not in-kernel.
 *
 * RMW_NEUTRALISED -- the correction was performed during a
 *                  read-modify-write transit (write path) rather
 *                  than on a user-initiated read. The decoded
 *                  data is about to be overwritten by the encode
 *                  step that follows in the same RMW cycle, so
 *                  the flip is *silently neutralised* before any
 *                  consumer observes it. This event would have
 *                  caused a read-side correction (or worse, an
 *                  uncorrectable) had the same disk-block been
 *                  read in isolation. Discriminating this flag
 *                  from the bare correctable event is what makes
 *                  the v3 paper's neutralisation-vs-correction
 *                  distinction empirically measurable from the
 *                  RS journal alone. Orthogonal to ENTROPY_VALID:
 *                  an RMW_NEUTRALISED event with >= 2 corrected
 *                  symbols still carries a valid Shannon entropy
 *                  estimate over the position list.
 *
 * Bits (1U << 3) and higher are reserved and MUST be zero on write.
 */
#define BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID  (1U << 0)
#define BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE  (1U << 1)
#define BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED (1U << 2)

/*
 * Shannon entropy parameters for the RS journal forensic estimator.
 *
 * BINS    -- number of histogram bins over the codeword position range.
 *            Power of 2 chosen so that bin_index = pos * BINS / code_len
 *            fits in u32 arithmetic without overflow for any beamfs
 *            codeword length (max 239 bytes).
 * Q       -- fractional bits in the Q-format LUT (Q16.16 = 16 frac bits).
 *
 * The full entropy LUT is defined in edac.c, generated reproducibly
 * by tools/gen_entropy_lut.py. See Documentation/format-v4.md.
 */
#define BEAMFS_RS_ENTROPY_BINS              8
#define BEAMFS_RS_ENTROPY_Q                 16

/*
 * Sentinel block number for journal entries that record a SUPERBLOCK
 * RS recovery rather than a data/metadata block recovery. The low 13
 * bits encode the SB sub-block index (0..12, < BEAMFS_SB_RS_SUBBLOCKS).
 *
 * Forensic decoder: if (re_block_no & BEAMFS_RS_BLOCK_NO_SB_MASK) ==
 *                       BEAMFS_RS_BLOCK_NO_SB_MARKER, the entry refers
 * to SB sub-block (re_block_no & BEAMFS_RS_BLOCK_NO_SB_IDX_MASK).
 *
 * The marker is chosen at the top of the u64 range, well above any
 * realistic block number on a filesystem (2^64 blocks * 4 KiB =
 * 2^76 bytes = 64 ZiB, unreachable by current and foreseeable storage).
 */
#define BEAMFS_RS_BLOCK_NO_SB_MARKER    0xFFFFFFFFFFFFF000ULL
#define BEAMFS_RS_BLOCK_NO_SB_MASK      0xFFFFFFFFFFFFF000ULL
#define BEAMFS_RS_BLOCK_NO_SB_IDX_MASK  0x0000000000000FFFULL

/*
 * On-disk format versions.
 *
 *   v2 -- bitmap RS FEC (introduced 2026-04-17)
 *   v3 -- extension points: feature bitmaps + data protection scheme
 *   v4 -- per-event Shannon entropy in RS journal; enlarged
 *         beamfs_rs_event (24 -> 40 bytes); enlarged SB RS layout
 *         (8 -> 13 subblocks). See Documentation/format-v4.md.
 *
 * Mount policy: strict equality with BEAMFS_VERSION_CURRENT.
 * As of Phase 1 sub-1.D, BEAMFS_VERSION_CURRENT = V5 (v5.0 minimal
 * RFC-able, mainline target). v1 images created by mkfs.beamfs
 * pre-Phase-1 are NOT mountable; they require offline reformat via
 * `mkfs.beamfs --profile=embedded`. Dual-format in-kernel parsing
 * is intentionally avoided (doubles audit surface for KASAN /
 * syzkaller for no operational benefit on a niche FS).
 *
 * Volumes created with mkfs.ftrfs (different magic) are NOT
 * mountable as beamfs by design (distinct filesystem).
 */
#define BEAMFS_VERSION_V1        1
#define BEAMFS_VERSION_V5        5
#define BEAMFS_VERSION_CURRENT   BEAMFS_VERSION_V5

/*
 * Data protection scheme values for s_data_protection_scheme.
 *
 * NONE              -- no FEC on data blocks (legacy behaviour, deprecated).
 * INODE_OPT_IN      -- RS FEC enabled per-inode via BEAMFS_INODE_FL_RS_ENABLED.
 *                     This is the v0.1.0 baseline behaviour. Deprecated by
 *                     threat model 6.3 (see Documentation/threat-model.md).
 * UNIVERSAL_INLINE  -- RS parity bytes embedded inline within each data block.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_SHADOW  -- RS parity stored in a dedicated out-of-band region.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_EXTENT  -- RS parity attached as an extent-based filesystem
 *                     attribute. Reserved for stage 4 of the staged plan.
 *
 * The kernel range-checks this field at mount and refuses values above
 * BEAMFS_DATA_PROTECTION_MAX. Three unused upper bytes of the __le32 act
 * as a structural sentinel: any single-byte corruption in the high-order
 * bytes produces a value outside the valid range and is rejected.
 */
#define BEAMFS_DATA_PROTECTION_NONE              0
#define BEAMFS_DATA_PROTECTION_INODE_OPT_IN      1
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE  2
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_SHADOW  3
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_EXTENT  4

/*
 * Per-data-block integrity field type (format v6, DATA_CSUM feature).
 * Stored in the block tail pad descriptor; see format-v6.md section 3.
 * CRC32 reuses beamfs_crc32 (crc32_le), the primitive backing i_crc32
 * and s_crc32. Values 2..255 reserved for keyed integrity (Family B).
 */
#define BEAMFS_CSUM_NONE   0   /* no integrity field present            */
#define BEAMFS_CSUM_CRC32  1   /* beamfs_crc32 over decoded user payload */
#define BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL   5
#define BEAMFS_DATA_PROTECTION_MAX               BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL

/*
 * Feature flag masks.
 *
 * s_feat_compat     -- informational flags. Unknown bits are logged but
 *                     do not prevent mount.
 * s_feat_incompat   -- structural format extensions. Unknown bits cause
 *                     mount to be refused (read or write).
 * s_feat_ro_compat  -- features that prevent safe write. Unknown bits cause
 *                     mount to be forced read-only with a warning.
 *
 * In BEAMFS_VERSION_V5 the bit layout below is reserved. None of these
 * features are active in v5.0 (RFC profile = embedded, all SUPP = 0).
 * Subsequent patches will activate flags one by one and update the
 * corresponding SUPP mask.
 */

/* COMPAT features (informational, mount continues if unknown) */
#define BEAMFS_FEATURE_COMPAT_RS_JOURNAL_VERBOSE  (1ULL << 0)
#define BEAMFS_FEATURE_COMPAT_LABEL_LONG          (1ULL << 1)
#define BEAMFS_FEATURE_COMPAT_DIR_INDEX           (1ULL << 2)

/* RO_COMPAT features (force RO mount if unknown) */
#define BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE       (1ULL << 0)
#define BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE        (1ULL << 1)
#define BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE      (1ULL << 2)
#define BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR        (1ULL << 3)
#define BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM        (1ULL << 4)
#define BEAMFS_FEATURE_RO_COMPAT_DATA_SELFID      (1ULL << 5)
					  /* per-data-block integrity field in
					   * the block tail pad; see format-v6.md.
					   * NOT in _SUPP until read/write path
					   * lands (declaration only).
					   */

/* INCOMPAT features (refuse mount if unknown) */
#define BEAMFS_FEATURE_INCOMPAT_EXTENTS           (1ULL << 0)
#define BEAMFS_FEATURE_INCOMPAT_64BIT             (1ULL << 1)
#define BEAMFS_FEATURE_INCOMPAT_BIGALLOC          (1ULL << 2)
#define BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS      (1ULL << 3)
#define BEAMFS_FEATURE_INCOMPAT_BTREE_ALLOC       (1ULL << 4)
#define BEAMFS_FEATURE_INCOMPAT_JOURNAL           (1ULL << 5)
#define BEAMFS_FEATURE_INCOMPAT_DAX               (1ULL << 6)
#define BEAMFS_FEATURE_INCOMPAT_RS_HEAVY          (1ULL << 7)
#define BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS      (1ULL << 8)
#define BEAMFS_FEATURE_INCOMPAT_BG_RS_PARITY      (1ULL << 9)
#define BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK       (1ULL << 10)
#define BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY     (1ULL << 11)
/*
 * BEAMFS_FEATURE_INCOMPAT_ENCRYPT: reserved for a future at-rest
 * encryption layer, not yet implemented. Declared now, alongside
 * SHADOW_PARITY and LARGE_BLOCK above, so an on-disk bit position is
 * fixed before any volume exists that could claim it by accident.
 *
 * Composition order, should this be implemented: encrypt -> RS encode
 * -> DATA_CSUM / DATA_SELFID, mirrored in reverse on read (RS decode
 * -> verify -> decrypt). Two consequences follow from that order:
 *
 *   - DATA_CSUM and DATA_SELFID would cover ciphertext, not plaintext.
 *     A corrupted block still fails closed (the RS-decoded bytes no
 *     longer match the stored digest), but decryption happening after
 *     the check means garbage plaintext is never produced from a block
 *     that already failed integrity, which matches the fail-closed
 *     doctrine documented in format-v6.md section 3.3.
 *
 *   - Encrypting before RS means a single ciphertext bit flip can
 *     alter more plaintext bits than the corresponding cleartext flip
 *     would (block-cipher diffusion), which the RS correction budget
 *     was not sized against. This is a design question for whoever
 *     implements the bit, not an implementation detail to silently
 *     inherit from the current cleartext path.
 */
#define BEAMFS_FEATURE_INCOMPAT_ENCRYPT           (1ULL << 12)
/*
 * INDIRECT_PARITY -- RS parity for indirection blocks, held out of band.
 *
 * An indirect block is 512 raw __le64 pointers filling the whole 4096
 * bytes, with no room for a checksum. data-protection-design.md section
 * 6.1 closes the dominant failure mode -- a pointer landing outside the
 * data range or on a block the bitmap says is free -- and leaves one
 * open: a flipped pointer that happens to land on a block that is both
 * in range and allocated passes every check there is.
 *
 * The asymmetry is what makes this worth closing. A data block has
 * eight correctable symbols per 255-byte subblock. A double-indirect
 * pointer has none, and losing it costs 262144 blocks.
 *
 * Parity lives in a region of its own rather than in the block, so
 * BEAMFS_INDIRECT_PTRS stays 512 and no BEAMFS_MAX_IBLOCK_* moves --
 * the format churn section 6.1 argues against does not happen. The
 * region is sized for the worst case, every data block being indirect,
 * because a table mapping blocks to parity slots would itself be
 * metadata needing protection. Sixteen bytes per block is 0.4% of the
 * volume, fixed, with the slot computed directly from the block number.
 *
 * INCOMPAT rather than RO_COMPAT: a kernel that does not know about the
 * region would allocate indirect blocks without writing their parity,
 * leaving the volume looking protected while it is not.
 */
#define BEAMFS_FEATURE_INCOMPAT_INDIRECT_PARITY   (1ULL << 13)

/*
 * How much an indirect block costs to protect, chosen at mkfs time.
 *
 * The right answer depends on the deployment, which is why it is a
 * choice rather than a constant. A CubeSat with 8 GiB of MRAM at
 * several hundred euro per gigabyte will not spend 6% of it; a server
 * with spare terabytes will, and gets correction for the price.
 *
 * NONE  no protection. The section 6.1 residual stands: a flipped
 *       pointer landing in range and on an allocated block is
 *       indistinguishable from a valid one.
 *
 * CRC   one CRC32 per subblock, 64 bytes per block, ~1.5% of the
 *       volume. Detects without correcting, which turns the residual
 *       from silent corruption into a clean fail-closed error -- the
 *       contract beamfs states everywhere else.
 *
 * RS    full RS(255,239), 256 bytes per block, ~6% of the volume.
 *       Corrects up to 8 symbols per subblock, so a hit pointer is
 *       repaired and the file survives.
 *
 * The region is sized for the worst case in every mode: any data block
 * can become an indirect block, and a table mapping blocks to slots
 * would itself be metadata needing protection.
 */
enum beamfs_ind_parity_mode {
	BEAMFS_IND_PARITY_NONE = 0,
	BEAMFS_IND_PARITY_CRC  = 1,
	BEAMFS_IND_PARITY_RS   = 2,
	BEAMFS_IND_PARITY__MAX
};

/* Bytes of parity per indirect block, by mode. */
#define BEAMFS_IND_PARITY_CRC_BYTES  (BEAMFS_DATA_INLINE_SUBBLOCKS * sizeof(__le32))
#define BEAMFS_IND_PARITY_RS_BYTES   (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY)

/* Parity bytes per indirect block: one RS(255,239) codeword's worth. */
#define BEAMFS_IND_PARITY_BYTES   BEAMFS_RS_PARITY
/*
 * An indirect block is 4096 bytes, past the 255-byte RS codeword, so it
 * is split like a data block: 16 subblocks of 239 data bytes, with one
 * 16-byte parity set per subblock. That is 256 parity bytes per block.
 */
#define BEAMFS_IND_PARITY_PER_BLOCK  (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY)
/* Parity slots that fit in one 4096-byte region block. */
#define BEAMFS_IND_PARITY_PER_REGION_BLOCK  (BEAMFS_BLOCK_SIZE / BEAMFS_IND_PARITY_PER_BLOCK)

/* Flags supported by this kernel module.
 * PER_INODE_RS: per-inode RS(255,239) parity protection. The kernel
 * write path has always computed the parity on every inode write
 * (namei.c::beamfs_write_inode_raw); enabling this bit in SUPP
 * unlocks the read-side decoder under any s_data_protection_scheme.
 */
#define BEAMFS_FEAT_COMPAT_SUPP    0ULL
#define BEAMFS_FEAT_RO_COMPAT_SUPP (BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM | \
				    BEAMFS_FEATURE_RO_COMPAT_DATA_SELFID)
#define BEAMFS_FEAT_INCOMPAT_SUPP  (BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS | \
				    BEAMFS_FEATURE_INCOMPAT_INDIRECT_PARITY)

/*
 * On-disk superblock - block 0
 * Total size: fits in one 4096-byte block
 */
struct beamfs_super_block;

/*
 * beamfs_bitmap_blocks_count_from_flags -- number of on-disk bitmap blocks.
 *
 * Decoded from s_flags bits 0..15. 0 in the field means the volume was
 * formatted before multi-block bitmap support; treat as legacy single
 * bitmap block.
 */
static inline u32 beamfs_bitmap_blocks_count_from_flags(__le32 s_flags_le)
{
	u32 v = le32_to_cpu(s_flags_le) & BEAMFS_SB_FLAGS_BITMAP_BLOCKS_MASK;

	return v ? v : 1;
}

struct beamfs_super_block {
	__le32  s_magic;            /* BEAMFS_MAGIC */
	__le32  s_block_size;       /* Block size in bytes */
	__le64  s_block_count;      /* Total blocks */
	__le64  s_free_blocks;      /* Free blocks */
	__le64  s_inode_count;      /* Total inodes */
	__le64  s_free_inodes;      /* Free inodes */
	__le64  s_inode_table_blk;  /* Block where inode table starts */
	__le64  s_data_start_blk;   /* First data block */
	__le32  s_version;          /* Filesystem version */
	__le32  s_flags;            /* Flags */
	__le32  s_crc32;            /* CRC32 of this superblock */
	__u8    s_uuid[16];         /* UUID */
	__u8    s_label[32];        /* Volume label */
	 struct beamfs_rs_event s_rs_journal[BEAMFS_RS_JOURNAL_SIZE]; /* 2560 bytes */
	__u8    s_rs_journal_head;  /* next write index (ring buffer) */
	__le64  s_bitmap_blk;       /* On-disk block bitmap block number */
	__le64  s_feat_compat;      /* Compatible feature flags (informational) */
	__le64  s_feat_incompat;    /* Incompatible features: refuse mount if unknown bit set */
	__le64  s_feat_ro_compat;   /* RO-compat features: force RO mount if unknown bit set */
	__le32  s_data_protection_scheme; /* enum BEAMFS_DATA_PROTECTION_* */
	/*
	 * Indirection parity region, valid when INCOMPAT_INDIRECT_PARITY
	 * is set. Zero on volumes without the feature.
	 *
	 * Placed before the superblock's own parity zone, as the comment
	 * on BEAMFS_SB_RS_S_PAD_INDEX requires: new fields go into s_pad
	 * ahead of it, and the index recomputes from offsetof.
	 */
	__le64  s_ind_parity_blk;   /* first block of the parity region */
	__le32  s_ind_parity_len;   /* length of the region, in blocks */
	__le32  s_ind_parity_mode;  /* enum beamfs_ind_parity_mode */
	__u8    s_pad[1367];        /* Padding to 4096 bytes */
} __packed;

/*
 * On-disk inode
 * Size: 256 bytes
 *
 * Addressing capacity:
 *   direct  (12)  =              48 KiB
 *   indirect (1)  =               2 MiB
 *   dindirect (1) =               1 GiB
 *   tindirect (1) =             512 GiB
 *
 * uid/gid: __le32 to support uid > 65535 (standard kernel convention)
 * timestamps: __le64 nanoseconds (required for space mission precision)
 */
struct beamfs_inode {
	__le16  i_mode;             /* File mode */
	__le16  i_nlink;            /* Hard link count */
	__le32  i_uid;              /* Owner UID */
	__le32  i_gid;              /* Owner GID */
	__le64  i_size;             /* File size in bytes (64-bit, future-proof) */
	__le64  i_atime;            /* Access time (ns) */
	__le64  i_mtime;            /* Modification time (ns) */
	__le64  i_ctime;            /* Change time (ns) */
	__le32  i_flags;            /* Inode flags */
	__le32  i_crc32;            /* CRC32 of inode (excluding this field) */
	__le64  i_direct[BEAMFS_DIRECT_BLOCKS];    /* Direct block pointers */
	__le64  i_indirect;         /* Single indirect (~2 MiB) */
	__le64  i_dindirect;        /* Double indirect (~1 GiB) */
	__le64  i_tindirect;        /* Triple indirect (~512 GiB) */
	__u8    i_reserved[84];     /* Padding to 256 bytes */
} __packed;

/*
 * Inode flags.
 *
 * BEAMFS_INODE_FL_RS_ENABLED: deprecated as of stage 3 (v0.3.0+).
 *   Was the per-inode opt-in for RS FEC under the
 *   BEAMFS_DATA_PROTECTION_INODE_OPT_IN scheme. Stage 3 replaces
 *   that scheme with BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL, where
 *   all inodes are RS-protected unconditionally. The flag is
 *   preserved in the bit definition so v0.1.0/v0.2.0 images that
 *   set it remain mountable; new images do not set it.
 *   See Documentation/threat-model.md section 6.1 and 6.3.
 */
#define BEAMFS_INODE_FL_RS_ENABLED   0x0001  /* deprecated, see comment */
#define BEAMFS_INODE_FL_VERIFIED     0x0002  /* Integrity verified */

/*
 * On-disk directory entry
 */
struct beamfs_dir_entry {
	__le64  d_ino;              /* Inode number */
	__le16  d_rec_len;          /* Record length */
	__u8    d_name_len;         /* Name length */
	__u8    d_file_type;        /* File type */
	char    d_name[BEAMFS_MAX_FILENAME + 1]; /* Filename */
} __packed;

/*
 * In-memory superblock info (stored in sb->s_fs_info)
 */

#endif /* _BEAMFS_FORMAT_H */
