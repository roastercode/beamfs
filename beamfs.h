/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * BEAMFS — Fault-Tolerant Radiation-Robust Filesystem
 * Based on: Fuchs, Langer, Trinitis — ARCS 2015
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef _BEAMFS_H
#define _BEAMFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/types.h>

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
 */
#define BEAMFS_SB_RS_COVERAGE_BYTES  2709   /* logical bytes (CRC32 range) */
#define BEAMFS_SB_RS_STAGING_BYTES   2743   /* 13 * BEAMFS_SB_RS_DATA_LEN   */
#define BEAMFS_SB_RS_DATA_LEN        211    /* per shortened subblock      */
#define BEAMFS_SB_RS_SUBBLOCKS       13     /* total subblocks             */
#define BEAMFS_SB_RS_PARITY_BYTES    208    /* 13 * BEAMFS_RS_PARITY        */
#define BEAMFS_SB_RS_PARITY_OFFSET   3888   /* end - parity bytes          */
#define BEAMFS_SB_RS_S_PAD_INDEX     (BEAMFS_SB_RS_PARITY_OFFSET - 2713)
                                          /* index in s_pad[]: 1175     */
#define BEAMFS_BITMAP_MAX_BLOCKS (BEAMFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/* Filesystem limits */
#define BEAMFS_MAX_FILENAME  255
#define BEAMFS_DIRECT_BLOCKS 12
#define BEAMFS_INDIRECT_BLOCKS 1
#define BEAMFS_DINDIRECT_BLOCKS 1

/*
 * Radiation Event Journal entry -- 40 bytes (v4 format).
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
	__le32  re_symbol_count;      /* 16..19  symbols corrected (renamed
	                                 *         from re_error_bits in v3) */
	__le32  re_entropy_q16_16;    /* 20..23  Shannon H, Q16.16, [0,3*65536) */
	__le32  re_flags;             /* 24..27  bit 0 = entropy_valid       */
	__le32  re_reserved;          /* 28..31  zero, structural sentinel   */
	__le32  re_crc32;             /* 32..35  CRC32 over bytes [0..32)    */
	__le32  re_pad;               /* 36..39  zero, alignment + sentinel  */
} __packed;                       /* 40 bytes */

#define BEAMFS_RS_JOURNAL_SIZE  64   /* entries in the radiation event journal */

/*
 * Flags for beamfs_rs_event::re_flags.
 *
 * ENTROPY_VALID -- the re_entropy_q16_16 field carries a meaningful
 *                  Shannon estimate. Cleared by zero-init (mkfs);
 *                  set by beamfs_log_rs_event() when entropy is computed
 *                  from the position list returned by RS decode.
 */
#define BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID  (1U << 0)

/*
 * Shannon entropy parameters for the RS journal forensic estimator.
 *
 * BINS    -- number of histogram bins over the codeword position range.
 *            Power of 2 chosen so that bin_index = pos * BINS / code_len
 *            fits in u32 arithmetic without overflow for any BEAMFS
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
 * BEAMFS is a fresh format (v1); no legacy v2/v3 images exist to
 * migrate from. Volumes created with mkfs.ftrfs (different magic)
 * are NOT mountable as BEAMFS by design (distinct filesystem).
 */
#define BEAMFS_VERSION_V1        1
#define BEAMFS_VERSION_CURRENT   BEAMFS_VERSION_V1

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
 * In BEAMFS_VERSION_V1 no feature bits are allocated; all three masks are
 * zero. Future features will allocate bits and update the corresponding
 * SUPP mask.
 */
#define BEAMFS_FEAT_COMPAT_SUPP      0ULL
#define BEAMFS_FEAT_INCOMPAT_SUPP    0ULL
#define BEAMFS_FEAT_RO_COMPAT_SUPP   0ULL

/*
 * On-disk superblock — block 0
 * Total size: fits in one 4096-byte block
 */
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
	 struct beamfs_rs_event s_rs_journal[BEAMFS_RS_JOURNAL_SIZE]; /* 1536 bytes */
	__u8    s_rs_journal_head;  /* next write index (ring buffer) */
	__le64  s_bitmap_blk;       /* On-disk block bitmap block number */
	__le64  s_feat_compat;      /* Compatible feature flags (informational) */
	__le64  s_feat_incompat;    /* Incompatible features: refuse mount if unknown bit set */
	__le64  s_feat_ro_compat;   /* RO-compat features: force RO mount if unknown bit set */
	__le32  s_data_protection_scheme; /* enum BEAMFS_DATA_PROTECTION_* */
	__u8    s_pad[1383];        /* Padding to 4096 bytes (v4 layout) */
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
struct beamfs_sb_info {
	/* Block allocator */
	unsigned long    *s_block_bitmap;  /* In-memory free block bitmap */
	unsigned long     s_nblocks;       /* Number of data blocks */
	unsigned long     s_data_start;    /* First data block number */
	/* Inode allocator */
	unsigned long    *s_inode_bitmap;  /* In-memory free inode bitmap */
	unsigned long     s_ninodes;       /* Total number of inodes */
	/* Superblock */
	struct beamfs_super_block *s_beamfs_sb; /* On-disk superblock copy */
	struct buffer_head       *s_sbh;      /* Buffer head for superblock */
	struct buffer_head       *s_bitmap_blkh; /* Buffer head for on-disk bitmap */
	spinlock_t                s_lock;     /* Superblock lock */
	unsigned long             s_free_blocks;
	unsigned long             s_free_inodes;
};

/*
 * In-memory inode info (embedded in VFS inode via container_of)
 */
struct beamfs_inode_info {
	__le64          i_direct[BEAMFS_DIRECT_BLOCKS];
	__le64          i_indirect;
	__le64          i_dindirect;
	__le64          i_tindirect;
	__u32           i_flags;
	struct inode    vfs_inode;  /* Must be last */
};

static inline struct beamfs_inode_info *BEAMFS_I(struct inode *inode)
{
	return container_of(inode, struct beamfs_inode_info, vfs_inode);
}

static inline struct beamfs_sb_info *BEAMFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* Function prototypes */
/* super.c */
int beamfs_fill_super(struct super_block *sb, struct fs_context *fc);
/*
 * beamfs_log_rs_event -- record a Reed-Solomon correction event in the
 *                       persistent superblock journal.
 *
 * @sb:           mounted superblock (sbi must be initialized)
 * @block_no:     block number where correction occurred, OR a SB
 *                sub-block sentinel (see BEAMFS_RS_BLOCK_NO_SB_MARKER)
 * @positions:    array of byte positions corrected within the codeword;
 *                MUST be non-NULL, MUST be valid for n_positions entries
 * @n_positions:  number of corrections; MUST be in [1, BEAMFS_RS_PARITY/2].
 *                Equal to the symbol count, written verbatim into
 *                re_symbol_count.
 * @code_len_bytes: data length of the codeword that produced @positions
 *                  (BEAMFS_INODE_RS_DATA for inodes, BEAMFS_SUBBLOCK_DATA
 *                  for bitmap subblocks, BEAMFS_SB_RS_DATA_LEN for SB
 *                  subblocks). Required by the entropy estimator to
 *                  compute the bin index from each byte position.
 *
 * Forensic policy:
 *   n_positions >= 2  -> entropy computed, ENTROPY_VALID flag set
 *   n_positions == 1  -> entropy zeroed, ENTROPY_VALID cleared
 *                        (single-sample event is not forensically
 *                        significant; Family A vs B distinction
 *                        relies on timestamp clustering for these
 *                        entries; see Documentation/format-v4.md).
 *
 * Computes the Shannon entropy estimate from @positions and stores it
 * in re_entropy_q16_16 with BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID set
 * (only when n_positions >= 2; see Forensic policy above).
 *
 * Invariants enforced via WARN_ON_ONCE; failures degrade gracefully
 * (no entropy logged) but never panic. Safe to call from any context
 * (spinlock-protected internally).
 */
void beamfs_log_rs_event(struct super_block *sb,
			u64 block_no,
			const int *positions,
			unsigned int n_positions,
			size_t code_len_bytes);
void beamfs_dirty_super(struct beamfs_sb_info *sbi);

/* inode.c */
struct inode *beamfs_iget(struct super_block *sb, unsigned long ino);
struct inode *beamfs_new_inode(struct inode *dir, umode_t mode);

/* dir.c */
extern const struct file_operations beamfs_dir_operations;
extern const struct inode_operations beamfs_dir_inode_operations;

/* file.c */
extern const struct file_operations beamfs_file_operations;
extern const struct inode_operations beamfs_file_inode_operations;
extern const struct address_space_operations beamfs_aops;

/* edac.c */
void beamfs_rs_init_tables(void);
void beamfs_rs_exit_tables(void);
__u32 beamfs_crc32(const void *buf, size_t len);
__u32 beamfs_crc32_sb(const struct beamfs_super_block *fsb);
int beamfs_rs_encode(u8 *data, size_t len, u8 *parity);

/*
 * beamfs_rs_decode -- decode and correct a shortened RS(255,239)
 *                    codeword in place, optionally exposing the
 *                    list of corrected byte positions for entropy.
 *
 * @data:         data bytes, corrected in place on success
 * @len:          number of data bytes (must match the encode call)
 * @parity:       parity bytes (BEAMFS_RS_PARITY)
 * @positions:    optional output, BEAMFS_RS_PARITY/2 = 8 entries; on a
 *                positive return holds the corrected byte positions
 *                (first n_corrected entries). May be NULL if the caller
 *                does not need the position list (no entropy logging).
 * @max_positions: capacity of @positions in entries; ignored if NULL.
 *
 * Returns:
 *   < 0  uncorrectable (-EBADMSG) or invalid input (-EINVAL)
 *   = 0  no errors detected
 *   > 0  number of symbol errors corrected in place
 *
 * Compute Shannon entropy: pass returned positions array to
 * beamfs_rs_compute_entropy_q16_16(positions, return_value, len).
 */
int beamfs_rs_decode(u8 *data, size_t len, u8 *parity,
		    int *positions, unsigned int max_positions);

/*
 * Shannon entropy estimator for the RS journal.
 *
 * @positions:       byte positions corrected (output of beamfs_rs_decode)
 * @n_positions:     number of valid entries in @positions, in [1, 8]
 * @code_len_bytes:  data length of the codeword, in [n_positions, 239]
 *
 * Returns H in Q16.16, range [0, 3*65536). Deterministic, no FPU,
 * no runtime division except for bin index computation. Backed by a
 * compile-time LUT generated by tools/gen_entropy_lut.py.
 *
 * Pre: positions != NULL, 1 <= n_positions <= BEAMFS_RS_PARITY/2
 *      code_len_bytes >= n_positions
 */
__u32 beamfs_rs_compute_entropy_q16_16(const int *positions,
				      unsigned int n_positions,
				      size_t code_len_bytes);

/*
 * Encode/decode N RS(255,239) shortened subblocks across a region.
 * data and parity may live in the same buffer (interleaved layout,
 * like the bitmap) or in two separate buffers (contiguous-parity
 * layout, like the superblock). The strides decouple the two cases.
 *
 * The decode helper additionally exposes per-subblock corrected
 * position lists for entropy logging. Each subblock writes up to
 * positions_stride entries into positions_buf at offset
 * i * positions_stride; the corresponding count is in results[i]
 * (= beamfs_rs_decode return value for that subblock).
 *
 * positions_buf may be NULL (and positions_stride == 0) if the caller
 * does not need entropy logging for any of the subblocks.
 */
int beamfs_rs_encode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks);
int beamfs_rs_decode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results,
			   int *positions_buf,
			   unsigned int positions_stride);

/* alloc.c */
int  beamfs_setup_bitmap(struct super_block *sb);
int  beamfs_write_bitmap(struct super_block *sb);
void beamfs_destroy_bitmap(struct super_block *sb);
u64  beamfs_alloc_block(struct super_block *sb);
void beamfs_free_block(struct super_block *sb, u64 block);
u64  beamfs_alloc_inode_num(struct super_block *sb);
void beamfs_free_inode_num(struct super_block *sb, u64 ino);

/* dir.c */
struct dentry *beamfs_lookup(struct inode *dir, struct dentry *dentry,
			    unsigned int flags);

/* namei.c */
int beamfs_write_inode(struct inode *inode, struct writeback_control *wbc);
int beamfs_write_inode_raw(struct inode *inode);

#endif /* _BEAMFS_H */
