// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAMFS — Superblock operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include "beamfs.h"

/* Inode cache (slab allocator) */
static struct kmem_cache *beamfs_inode_cachep;

/*
 * alloc_inode — allocate a new inode with beamfs_inode_info embedded
 */
static struct inode *beamfs_alloc_inode(struct super_block *sb)
{
	struct beamfs_inode_info *fi;

	fi = kmem_cache_alloc(beamfs_inode_cachep, GFP_KERNEL);
	if (!fi)
		return NULL;

	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_tindirect = 0;
	fi->i_flags     = 0;

	return &fi->vfs_inode;
}

/*
 * free_inode — return inode to slab cache (kernel 5.9+ uses free_inode)
 */
static void beamfs_free_inode(struct inode *inode)
{
	kmem_cache_free(beamfs_inode_cachep, BEAMFS_I(inode));
}

/*
 * statfs — filesystem statistics
 */
static int beamfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block   *sb  = dentry->d_sb;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	buf->f_type    = BEAMFS_MAGIC;
	buf->f_bsize   = sb->s_blocksize;
	buf->f_blocks  = le64_to_cpu(sbi->s_beamfs_sb->s_block_count);
	buf->f_bfree   = sbi->s_free_blocks;
	buf->f_bavail  = sbi->s_free_blocks;
	buf->f_files   = le64_to_cpu(sbi->s_beamfs_sb->s_inode_count);
	buf->f_ffree   = sbi->s_free_inodes;
	buf->f_namelen = BEAMFS_MAX_FILENAME;

	return 0;
}

/*
 * put_super — release superblock resources
 */
static void beamfs_put_super(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (sbi) {
		beamfs_destroy_bitmap(sb);
		brelse(sbi->s_sbh);
		kfree(sbi->s_beamfs_sb);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

/*
 * evict_inode — called when inode nlink drops to 0 and last reference released
 * Frees the inode number back to the bitmap.
 */
/*
 * beamfs_free_data_blocks -- release all data blocks of a deleted inode.
 *
 * Frees direct blocks and the single indirect block (and all blocks
 * it points to). Called from evict_inode when nlink drops to 0.
 */
static void beamfs_free_data_blocks(struct inode *inode)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block      *sb = inode->i_sb;
	int i;

	/* Free direct blocks */
	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		u64 blk = le64_to_cpu(fi->i_direct[i]);

		if (blk) {
			beamfs_free_block(sb, blk);
			fi->i_direct[i] = 0;
		}
	}

	/* Free single indirect block and all blocks it points to */
	if (fi->i_indirect) {
		u64 indirect_blk = le64_to_cpu(fi->i_indirect);
		struct buffer_head *ibh = sb_bread(sb, indirect_blk);

		if (ibh) {
			__le64 *ptrs = (__le64 *)ibh->b_data;
			u64 nptrs = BEAMFS_BLOCK_SIZE / sizeof(__le64);
			u64 j;

			for (j = 0; j < nptrs; j++) {
				u64 blk = le64_to_cpu(ptrs[j]);

				if (blk)
					beamfs_free_block(sb, blk);
			}
			brelse(ibh);
		}
		beamfs_free_block(sb, indirect_blk);
		fi->i_indirect = 0;
	}
}

static void beamfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	/*
	 * If the file is truly deleted (nlink == 0), free all data blocks,
	 * zero i_mode on disk so the inode table scan at next mount
	 * correctly identifies this slot as free, then release the inode
	 * number back to the bitmap.
	 */
	if (!inode->i_nlink) {
		beamfs_free_data_blocks(inode);
		inode->i_mode = 0;
		beamfs_write_inode_raw(inode);
		beamfs_free_inode_num(inode->i_sb, (u64)inode->i_ino);
	}
	clear_inode(inode);
}

static const struct super_operations beamfs_super_ops = {
	.alloc_inode    = beamfs_alloc_inode,
	.free_inode     = beamfs_free_inode,
	.evict_inode    = beamfs_evict_inode,
	.put_super      = beamfs_put_super,
	.write_inode    = beamfs_write_inode,
	.statfs         = beamfs_statfs,
};

/*
 * beamfs_dirty_super - propagate the authoritative in-memory superblock
 * (sbi->s_beamfs_sb) onto the buffer head, recompute s_crc32, and mark
 * the buffer dirty for writeback.
 *
 * Every site that mutates the on-disk superblock (free_blocks,
 * free_inodes, RS journal, ...) must call this helper instead of
 * mark_buffer_dirty(sbi->s_sbh) directly. Without the CRC refresh,
 * the on-disk superblock keeps a stale checksum that fails verification
 * at the next mount.
 *
 * Caller MUST hold sbi->s_lock so that the snapshot copied to the
 * buffer head is taken atomically with respect to other writers.
 */
/*
 * beamfs_sb_to_rs_staging -- serialize the CRC32-covered region of a
 * superblock into a contiguous staging buffer for RS encode/decode.
 *
 * Output buffer layout (BEAMFS_SB_RS_STAGING_BYTES bytes):
 *   [0, off_crc32)                              region A:
 *                                                 sb_bytes[0..off_crc32)
 *   [off_crc32, BEAMFS_SB_RS_COVERAGE_BYTES)     region B:
 *                                                 sb_bytes[off_uuid..off_pad)
 *   [BEAMFS_SB_RS_COVERAGE_BYTES,
 *    BEAMFS_SB_RS_STAGING_BYTES)                 zero pad to round up
 *                                                 to whole RS subblocks
 *
 * Field offsets are derived from struct layout via offsetof, so the
 * helper is invariant under future format extensions provided
 * BEAMFS_SB_RS_COVERAGE_BYTES is updated in lockstep. BUILD_BUG_ON
 * below enforces that consistency at compile time.
 *
 * The s_crc32 field [off_crc32, off_uuid) is excluded, exactly as
 * beamfs_crc32_sb() does. Same coverage on both protection layers.
 *
 * Must match mkfs.beamfs.c::sb_to_rs_staging() byte-for-byte.
 */
static void beamfs_sb_to_rs_staging(const struct beamfs_super_block *sb,
				   u8 staging[BEAMFS_SB_RS_STAGING_BYTES])
{
	const u8 *base = (const u8 *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);
	BUILD_BUG_ON(BEAMFS_SB_RS_STAGING_BYTES <
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	memcpy(staging, base, off_crc32);
	memcpy(staging + off_crc32, base + off_uuid, off_pad - off_uuid);
	memset(staging + BEAMFS_SB_RS_COVERAGE_BYTES, 0,
	       BEAMFS_SB_RS_STAGING_BYTES - BEAMFS_SB_RS_COVERAGE_BYTES);
}

/*
 * beamfs_sb_from_rs_staging -- inverse of beamfs_sb_to_rs_staging.
 * Restores the (possibly RS-corrected) bytes from staging back onto
 * the superblock, leaving s_crc32 (bytes [off_crc32, off_uuid))
 * untouched. The trailing zero-pad bytes of staging
 * [BEAMFS_SB_RS_COVERAGE_BYTES, BEAMFS_SB_RS_STAGING_BYTES) are not
 * copied back.
 *
 * Used on the mount-time RS recovery path in beamfs_fill_super.
 */
static void beamfs_sb_from_rs_staging(const u8 staging[BEAMFS_SB_RS_STAGING_BYTES],
				     struct beamfs_super_block *sb)
{
	u8 *base = (u8 *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	memcpy(base, staging, off_crc32);
	memcpy(base + off_uuid, staging + off_crc32, off_pad - off_uuid);
}

void beamfs_dirty_super(struct beamfs_sb_info *sbi)
{
	struct beamfs_super_block *fsb;
	u32 crc;

	if (!sbi || !sbi->s_sbh || !sbi->s_beamfs_sb)
		return;

	fsb = (struct beamfs_super_block *)sbi->s_sbh->b_data;

	/*
	 * Copy the authoritative in-memory image onto the buffer head.
	 * Several callers update sbi->s_beamfs_sb in place without touching
	 * fsb; this memcpy serializes them onto disk.
	 */
	memcpy(fsb, sbi->s_beamfs_sb, sizeof(*fsb));

	/* Encode RS parity over CRC32-covered region (skipping s_crc32). */
	{
		u8 staging[BEAMFS_SB_RS_STAGING_BYTES];
		u8 *parity_dst = (u8 *)fsb + BEAMFS_SB_RS_PARITY_OFFSET;

		beamfs_sb_to_rs_staging(fsb, staging);
		beamfs_rs_encode_region(staging, BEAMFS_SB_RS_DATA_LEN,
				       parity_dst, BEAMFS_RS_PARITY,
				       BEAMFS_SB_RS_DATA_LEN,
				       BEAMFS_SB_RS_SUBBLOCKS);
	}

	crc = beamfs_crc32_sb(fsb);
	fsb->s_crc32 = cpu_to_le32(crc);
	sbi->s_beamfs_sb->s_crc32 = fsb->s_crc32;

	mark_buffer_dirty(sbi->s_sbh);
}

/*
 * beamfs_log_rs_event -- record an RS correction event in the superblock
 *                       persistent journal (v4 format, 40-byte entry).
 *
 * See beamfs.h for full parameter contract. Forensic policy summary:
 *   n_positions >= 2 -> Shannon entropy computed, ENTROPY_VALID set
 *   n_positions == 1 -> entropy zeroed, ENTROPY_VALID cleared
 *
 * Safe to call from any context; spinlock-protected internally.
 */
void beamfs_log_rs_event(struct super_block *sb,
			u64 block_no,
			const int *positions,
			unsigned int n_positions,
			size_t code_len_bytes)
{
	struct beamfs_sb_info  *sbi = BEAMFS_SB(sb);
	struct beamfs_rs_event *ev;
	u8 head;
	u32 entropy_q16 = 0;
	u32 flags = 0;

	if (!sbi || !sbi->s_sbh)
		return;

	/* Invariant guards: surface bug at WARN_ON_ONCE without panicking.
	 * The journal entry is silently skipped if invariants are violated. */
	if (WARN_ON_ONCE(!positions))
		return;
	if (WARN_ON_ONCE(n_positions == 0 ||
			 n_positions > BEAMFS_RS_PARITY / 2))
		return;
	if (WARN_ON_ONCE(code_len_bytes == 0 ||
			 code_len_bytes > BEAMFS_SUBBLOCK_DATA))
		return;

	/* Forensic policy: single-sample event yields a mathematically
	 * defined but non-significant entropy (H=0). Per Documentation/
	 * format-v4.md and threat-model.md section 6.4, we clear the
	 * ENTROPY_VALID flag in that case so a forensic analyst sees
	 * "no usable entropy estimate, fall back to timestamp clustering". */
	if (n_positions >= 2) {
		entropy_q16 = beamfs_rs_compute_entropy_q16_16(positions,
							      n_positions,
							      code_len_bytes);
		flags = BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID;
	}

	spin_lock(&sbi->s_lock);

	head = sbi->s_beamfs_sb->s_rs_journal_head % BEAMFS_RS_JOURNAL_SIZE;
	ev   = &sbi->s_beamfs_sb->s_rs_journal[head];

	ev->re_block_no       = cpu_to_le64(block_no);
	ev->re_timestamp      = cpu_to_le64(ktime_get_ns());
	ev->re_symbol_count   = cpu_to_le32(n_positions);
	ev->re_entropy_q16_16 = cpu_to_le32(entropy_q16);
	ev->re_flags          = cpu_to_le32(flags);
	ev->re_reserved       = 0; /* structural sentinel, MUST stay zero */
	ev->re_pad            = 0; /* structural sentinel, MUST stay zero */
	{
		u32 ev_crc = beamfs_crc32(ev,
					 offsetof(struct beamfs_rs_event,
						  re_crc32));
		ev->re_crc32 = cpu_to_le32(ev_crc);
	}

	sbi->s_beamfs_sb->s_rs_journal_head = (head + 1) % BEAMFS_RS_JOURNAL_SIZE;

	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	pr_debug("beamfs: RS correction block=%llu symbols=%u entropy_valid=%u entropy_q16=%u\n",
		 block_no, n_positions,
		 (flags & BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID) ? 1 : 0,
		 entropy_q16);
}

/*
 * Pending RS recovery event captured during the SB CRC32-failure path,
 * to be replayed into the journal once sbi is fully initialized.
 *
 * The capture-then-replay pattern (Option 3) decouples the recovery
 * detection from the journal write: it preserves the fail-secure
 * invariant (validate the SB image before allocating sbi) while
 * ensuring forensic events from SB recovery are not lost. See
 * Documentation/format-v4.md "Stage 3 item 4 fill_super event flow".
 */
struct beamfs_pending_rs_event {
	u64 block_no;
	unsigned int n_positions;
	size_t code_len_bytes;
	int positions[BEAMFS_RS_PARITY / 2];
};

/*
 * beamfs_fill_super — read superblock from disk and initialize VFS sb
 */
int beamfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct beamfs_sb_info     *sbi;
	struct beamfs_super_block *fsb;
	struct buffer_head       *bh;
	struct inode             *root_inode;
	__u32                     crc;
	struct beamfs_pending_rs_event *pending = NULL;
	unsigned int              n_pending = 0;
	int                       ret = -EINVAL;

	/* Set block size */
	if (!sb_set_blocksize(sb, BEAMFS_BLOCK_SIZE)) {
		errorf(fc, "beamfs: unable to set block size %d", BEAMFS_BLOCK_SIZE);
		return -EINVAL;
	}

	/* Read block 0 — superblock */
	bh = sb_bread(sb, 0);
	if (!bh) {
		errorf(fc, "beamfs: unable to read superblock");
		return -EIO;
	}

	fsb = (struct beamfs_super_block *)bh->b_data;

	/* Verify magic */
	if (le32_to_cpu(fsb->s_magic) != BEAMFS_MAGIC) {
		errorf(fc, "beamfs: bad magic 0x%08x (expected 0x%08x)",
		       le32_to_cpu(fsb->s_magic), BEAMFS_MAGIC);
		goto out_brelse;
	}

	/* Strict version check: this kernel mounts only BEAMFS_VERSION_CURRENT
	 * images. Older v2/v3 images require offline migration via mkfs.beamfs
	 * --migrate. Rationale: dual-format in-kernel parsing doubles the audit
	 * surface (KASAN, syzkaller) for no operational benefit on a niche FS. */
	if (le32_to_cpu(fsb->s_version) != BEAMFS_VERSION_CURRENT) {
		errorf(fc, "beamfs: unsupported on-disk version %u (this kernel requires v%u)",
		       le32_to_cpu(fsb->s_version), BEAMFS_VERSION_CURRENT);
		goto out_brelse;
	}

	/* Verify CRC32 of superblock (excluding the crc32 field itself) */
	crc = beamfs_crc32_sb(fsb);
	if (crc != le32_to_cpu(fsb->s_crc32)) {
		u8 staging[BEAMFS_SB_RS_STAGING_BYTES];
		u8 *parity_src = (u8 *)fsb + BEAMFS_SB_RS_PARITY_OFFSET;
		int rs_results[BEAMFS_SB_RS_SUBBLOCKS];
		int rs_positions[BEAMFS_SB_RS_SUBBLOCKS *
				 (BEAMFS_RS_PARITY / 2)];
		int rc;

		pr_warn("beamfs: superblock CRC32 mismatch (got 0x%08x, expected 0x%08x), attempting RS recovery\n",
			crc, le32_to_cpu(fsb->s_crc32));

		beamfs_sb_to_rs_staging(fsb, staging);
		rc = beamfs_rs_decode_region(staging, BEAMFS_SB_RS_DATA_LEN,
					    parity_src, BEAMFS_RS_PARITY,
					    BEAMFS_SB_RS_DATA_LEN,
					    BEAMFS_SB_RS_SUBBLOCKS,
					    rs_results,
					    rs_positions,
					    BEAMFS_RS_PARITY / 2);
		if (rc < 0) {
			errorf(fc, "beamfs: superblock CRC32 mismatch and RS uncorrectable");
			goto out_brelse;
		}

		beamfs_sb_from_rs_staging(staging, fsb);

		crc = beamfs_crc32_sb(fsb);
		if (crc != le32_to_cpu(fsb->s_crc32)) {
			errorf(fc, "beamfs: superblock CRC32 still mismatch after RS recovery");
			goto out_brelse;
		}

		pr_warn("beamfs: superblock corrected by RS FEC\n");

		/* Capture per-subblock recovery events for deferred replay
		 * into the journal once sbi is initialized. Allocation
		 * failure here is non-fatal: the SB has already been
		 * corrected on disk, only the journal record is lost. */
		{
			unsigned int i;
			unsigned int n_events = 0;

			for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
				if (rs_results[i] > 0)
					n_events++;
			}

			if (n_events > 0) {
				pending = kmalloc_array(n_events,
							sizeof(*pending),
							GFP_KERNEL);
				if (!pending) {
					pr_warn("beamfs: cannot allocate pending events buffer; %u SB recovery events not journalled\n",
						n_events);
				} else {
					unsigned int k = 0;

					for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
						unsigned int np;
						int *src;

						if (rs_results[i] <= 0)
							continue;
						np = (unsigned int)rs_results[i];
						if (np > BEAMFS_RS_PARITY / 2)
							np = BEAMFS_RS_PARITY / 2;
						pending[k].block_no =
							BEAMFS_RS_BLOCK_NO_SB_MARKER |
							(u64)i;
						pending[k].n_positions = np;
						pending[k].code_len_bytes =
							BEAMFS_SB_RS_DATA_LEN;
						src = rs_positions +
							i * (BEAMFS_RS_PARITY / 2);
						memcpy(pending[k].positions,
						       src,
						       np * sizeof(int));
						k++;
					}
					n_pending = k;
				}
			}
		}
	}

	/*
	 * Validate v3 feature fields.
	 *
	 * s_data_protection_scheme: range-check against the enum maximum.
	 *   The three high-order bytes of the __le32 act as a structural
	 *   sentinel; any value above BEAMFS_DATA_PROTECTION_MAX is rejected.
	 * s_feat_incompat:  unknown bits are a hard refusal (any read).
	 * s_feat_ro_compat: unknown bits force SB_RDONLY but allow mount.
	 * s_feat_compat:    informational, never gates mount.
	 */
	{
		u32 scheme = le32_to_cpu(fsb->s_data_protection_scheme);
		u64 unknown_incompat  = le64_to_cpu(fsb->s_feat_incompat) &
					~BEAMFS_FEAT_INCOMPAT_SUPP;
		u64 unknown_ro_compat = le64_to_cpu(fsb->s_feat_ro_compat) &
					~BEAMFS_FEAT_RO_COMPAT_SUPP;
		u64 unknown_compat    = le64_to_cpu(fsb->s_feat_compat) &
					~BEAMFS_FEAT_COMPAT_SUPP;

		if (scheme > BEAMFS_DATA_PROTECTION_MAX) {
			errorf(fc, "beamfs: invalid data_protection_scheme %u (max %u)",
			       scheme, BEAMFS_DATA_PROTECTION_MAX);
			goto out_brelse;
		}

		if (unknown_incompat) {
			errorf(fc, "beamfs: unsupported incompat features 0x%016llx",
			       unknown_incompat);
			goto out_brelse;
		}

		if (unknown_ro_compat && !sb_rdonly(sb)) {
			pr_warn("beamfs: unsupported ro_compat features 0x%016llx, forcing read-only mount\n",
				unknown_ro_compat);
			sb->s_flags |= SB_RDONLY;
		}

		if (unknown_compat)
			pr_info("beamfs: unknown compat features 0x%016llx (informational)\n",
				unknown_compat);
	}

	/* Allocate in-memory sb info */
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		ret = -ENOMEM;
		goto out_brelse;
	}

	sbi->s_beamfs_sb = kzalloc(sizeof(*sbi->s_beamfs_sb), GFP_KERNEL);
	if (!sbi->s_beamfs_sb) {
		ret = -ENOMEM;
		goto out_free_sbi;
	}

	memcpy(sbi->s_beamfs_sb, fsb, sizeof(*fsb));
	sbi->s_sbh         = bh;
	sbi->s_free_blocks = le64_to_cpu(fsb->s_free_blocks);
	sbi->s_free_inodes = le64_to_cpu(fsb->s_free_inodes);
	spin_lock_init(&sbi->s_lock);

	sb->s_fs_info  = sbi;
	sb->s_magic    = BEAMFS_MAGIC;
	sb->s_op       = &beamfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/* Read root inode (inode 1) */
	root_inode = beamfs_iget(sb, 1);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		pr_err("beamfs: failed to read root inode: %d\n", ret);
		goto out_free_fsb;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}

	/* Replay any SB RS recovery events captured before sbi was ready.
	 * These are journalled before bitmap setup so the forensic order
	 * (SB events first, then bitmap events) reflects mount sequence. */
	if (pending) {
		unsigned int i;

		for (i = 0; i < n_pending; i++) {
			beamfs_log_rs_event(sb,
					   pending[i].block_no,
					   pending[i].positions,
					   pending[i].n_positions,
					   pending[i].code_len_bytes);
		}
		kfree(pending);
		pending = NULL;
	}

	if (beamfs_setup_bitmap(sb)) {
		ret = -ENOMEM;
		goto out_put_root;
	}

	pr_info("beamfs: mounted v%u (blocks=%llu free=%lu inodes=%llu scheme=%u feat=0x%016llx/0x%016llx/0x%016llx)\n",
		le32_to_cpu(fsb->s_version),
		le64_to_cpu(fsb->s_block_count),
		sbi->s_free_blocks,
		le64_to_cpu(fsb->s_inode_count),
		le32_to_cpu(fsb->s_data_protection_scheme),
		le64_to_cpu(fsb->s_feat_compat),
		le64_to_cpu(fsb->s_feat_incompat),
		le64_to_cpu(fsb->s_feat_ro_compat));

	return 0;

out_put_root:
	dput(sb->s_root);
	sb->s_root = NULL;
out_free_fsb:
	kfree(sbi->s_beamfs_sb);
out_free_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
out_brelse:
	/* Free any pending events buffer that survived to here. The replay
	 * block sets pending = NULL after consumption, so kfree(NULL) is the
	 * nominal no-op. Reaching here with pending != NULL means a failure
	 * occurred between capture and replay; the events are dropped. */
	kfree(pending);
	brelse(bh);
	return ret;
}

/*
 * fs_context ops — kernel 5.15+ mount API
 */
static int beamfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, beamfs_fill_super);
}

/*
 * beamfs_reconfigure — handle mount -o remount
 *
 * xfstests calls remount,ro after each test to verify filesystem
 * integrity. BEAMFS accepts the reconfigure request without
 * taking any action — ro/rw transitions are handled by the VFS.
 */
static int beamfs_reconfigure(struct fs_context *fc)
{
	return 0;
}

static const struct fs_context_operations beamfs_context_ops = {
	.get_tree   = beamfs_get_tree,
	.reconfigure = beamfs_reconfigure,
};

static int beamfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &beamfs_context_ops;
	return 0;
}

static struct file_system_type beamfs_fs_type = {
	.owner            = THIS_MODULE,
	.name             = "beamfs",
	.init_fs_context  = beamfs_init_fs_context,
	.kill_sb          = kill_block_super,
	.fs_flags         = FS_REQUIRES_DEV,
};

/*
 * Inode cache constructor
 */
static void beamfs_inode_init_once(void *obj)
{
	struct beamfs_inode_info *fi = obj;

	inode_init_once(&fi->vfs_inode);
}

/*
 * Module init / exit
 */
static int __init beamfs_init(void)
{
	int ret;

	/* Verify on-disk structure sizes at compile time */
	BUILD_BUG_ON(sizeof(struct beamfs_super_block) != BEAMFS_BLOCK_SIZE);
	BUILD_BUG_ON(sizeof(struct beamfs_inode) != 256);
	BUILD_BUG_ON(sizeof(struct beamfs_rs_event) != 40);
	BUILD_BUG_ON(sizeof(struct beamfs_dir_entry) != 268);

	/* Initialize GF(2^8) tables for RS FEC — once, before any mount */
	beamfs_rs_init_tables();

	beamfs_inode_cachep =
		kmem_cache_create("beamfs_inode_cache",
				  sizeof(struct beamfs_inode_info),
				  0,
				  SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				  beamfs_inode_init_once);

	if (!beamfs_inode_cachep) {
		pr_err("beamfs: failed to create inode cache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&beamfs_fs_type);
	if (ret) {
		pr_err("beamfs: failed to register filesystem: %d\n", ret);
		kmem_cache_destroy(beamfs_inode_cachep);
		return ret;
	}

	pr_info("beamfs: module loaded (BEAMFS Fault-Tolerant Radiation-Robust FS)\n");
	return 0;
}

static void __exit beamfs_exit(void)
{
	unregister_filesystem(&beamfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(beamfs_inode_cachep);
	beamfs_rs_exit_tables();
	pr_info("beamfs: module unloaded\n");
}

module_init(beamfs_init);
module_exit(beamfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aurelien DESBRIERES <aurelien@hackers.camp>");
MODULE_DESCRIPTION("BEAMFS: Beam-Resilient Filesystem");
MODULE_VERSION("0.1.0");
MODULE_ALIAS_FS("beamfs");
MODULE_SOFTDEP("pre: reed_solomon");
