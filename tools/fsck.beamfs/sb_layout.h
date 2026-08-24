/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck.beamfs - On-disk structure mirrors (userland)
 *
 * Byte-exact mirrors of the kernel beamfs.h on-disk structures.
 * Used by fsck.beamfs to parse the on-disk superblock from a raw
 * 4096-byte block read directly from the block device.
 *
 * Must match the kernel beamfs.h byte-for-byte. Any change in
 * kernel beamfs.h that affects on-disk layout MUST be mirrored
 * here in lockstep.
 *
 * Sub-phase 2 (Pass 1 superblock check) prerequisite headers.
 * The actual decoder + pass1 implementation lands in the next
 * commit.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef FSCK_BEAMFS_SB_LAYOUT_H
#define FSCK_BEAMFS_SB_LAYOUT_H

#include <assert.h>
#include <stdint.h>

/* Magic and dimensional invariants (kernel beamfs.h) */
#define BEAMFS_MAGIC               0x4245414D
#define BEAMFS_BLOCK_SIZE          4096
#define BEAMFS_RS_JOURNAL_SIZE     64

/* SB Reed-Solomon protection geometry (kernel beamfs.h) */
#define BEAMFS_SB_RS_COVERAGE_BYTES  2709
#define BEAMFS_SB_RS_DATA_LEN        211
#define BEAMFS_SB_RS_SUBBLOCKS       13
#define BEAMFS_SB_RS_PARITY_BYTES    208
#define BEAMFS_SB_RS_PARITY_OFFSET   3888
#define BEAMFS_SB_RS_STAGING_BYTES   2743   /* 13 * BEAMFS_SB_RS_DATA_LEN */

/*
 * Electromagnetic Resilience Journal entry -- 40 bytes (v4 format).
 * Byte-exact mirror of struct beamfs_rs_event in kernel beamfs.h.
 * mkfs.beamfs writes the on-disk image; fsck.beamfs reads it.
 */
struct beamfs_rs_event {
	uint64_t re_block_no;          /*  0..7   */
	uint64_t re_timestamp;         /*  8..15  */
	uint32_t re_symbol_count;      /* 16..19  */
	uint32_t re_entropy_q16_16;    /* 20..23  Shannon H, Q16.16 */
	uint32_t re_flags;             /* 24..27  bit 0 = entropy_valid */
	uint32_t re_reserved;          /* 28..31  zero, structural sentinel */
	uint32_t re_crc32;             /* 32..35  CRC32 over bytes [0..32) */
	uint32_t re_pad;               /* 36..39  zero, alignment + sentinel */
} __attribute__((packed));
static_assert(sizeof(struct beamfs_rs_event) == 40,
	      "beamfs_rs_event must be 40 bytes (v4 format)");

/*
 * On-disk superblock (block 0) -- 4096 bytes total.
 * Byte-exact mirror of struct beamfs_super_block in kernel beamfs.h.
 * Layout follows format-v5.md section 4.1 (embedded profile).
 */
struct beamfs_super_block {
	uint32_t s_magic;            /* BEAMFS_MAGIC */
	uint32_t s_block_size;       /* always 4096 */
	uint64_t s_block_count;
	uint64_t s_free_blocks;
	uint64_t s_inode_count;
	uint64_t s_free_inodes;
	uint64_t s_inode_table_blk;
	uint64_t s_data_start_blk;
	uint32_t s_version;          /* BEAMFS_VERSION_CURRENT = 5 */
	uint32_t s_flags;            /* reserved, zero */
	uint32_t s_crc32;            /* CRC32 over coverage regions */
	uint8_t  s_uuid[16];
	uint8_t  s_label[32];
	struct beamfs_rs_event s_rs_journal[BEAMFS_RS_JOURNAL_SIZE];
	uint8_t  s_rs_journal_head;
	uint64_t s_bitmap_blk;
	uint64_t s_feat_compat;
	uint64_t s_feat_incompat;
	uint64_t s_feat_ro_compat;
	uint32_t s_data_protection_scheme;
	uint8_t  s_pad[1383];        /* padding to 4096; trailing 208 = SB RS parity */
} __attribute__((packed));
static_assert(sizeof(struct beamfs_super_block) == BEAMFS_BLOCK_SIZE,
	      "beamfs_super_block must be 4096 bytes");

#endif /* FSCK_BEAMFS_SB_LAYOUT_H */
