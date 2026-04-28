// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAMFS — Directory operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "beamfs.h"

/*
 * beamfs_readdir — iterate directory entries
 *
 * ctx->pos encoding:
 *   0, 1       : '.' and '..' (emitted by dir_emit_dots)
 *   INT_MAX    : EOF
 *   other      : ((block_idx + 1) << 16) | entry_slot
 *
 * This encoding allows correct resumption if getdents() is interrupted
 * mid-directory. block_idx is 0-based index into i_direct[]; entry_slot
 * is the entry index within that block.
 *
 * Maximum directory size: BEAMFS_DIRECT_BLOCKS blocks × (4096 / 268) entries
 * = 12 × 15 = 180 entries. block_idx fits in 15 bits, entry_slot in 8 bits,
 * well within the 32-bit pos space.
 */
static int beamfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode            *inode = file_inode(file);
	struct super_block      *sb    = inode->i_sb;
	struct beamfs_inode_info *fi    = BEAMFS_I(inode);
	struct buffer_head      *bh;
	struct beamfs_dir_entry  *de;
	unsigned long  block_no;
	unsigned int   offset;
	int            block_idx;
	int            entry_slot;
	int            start_block;
	int            start_slot;

	if (ctx->pos == INT_MAX)
		return 0;

	/* Emit . and .. */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return 0;
	}

	/*
	 * Decode resume position. pos == 2 means start from the beginning.
	 * pos > 2 encodes ((block_idx+1) << 16) | entry_slot from last emit.
	 */
	if (ctx->pos <= 2) {
		start_block = 0;
		start_slot  = 0;
	} else {
		start_block = (int)((ctx->pos >> 16) & 0x7FFF) - 1;
		start_slot  = (int)(ctx->pos & 0xFFFF);
	}

	for (block_idx = start_block; block_idx < BEAMFS_DIRECT_BLOCKS; block_idx++) {
		block_no = le64_to_cpu(fi->i_direct[block_idx]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		entry_slot = (block_idx == start_block) ? start_slot : 0;
		offset = entry_slot * sizeof(struct beamfs_dir_entry);

		while (offset + sizeof(*de) <= BEAMFS_BLOCK_SIZE) {
			de = (struct beamfs_dir_entry *)(bh->b_data + offset);

			/*
			 * Skip free slots (d_ino == 0): never-used trailing
			 * slots and slots freed by beamfs_del_dirent. The
			 * scan must traverse the entire block, not stop at
			 * the first hole, because live entries may follow
			 * a deleted entry within the same block.
			 */
			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.' &&
			      de->d_name[1] == '.')) {
				/*
				 * Update ctx->pos before dir_emit so the VFS
				 * has a unique seek offset for each entry.
				 * Encode as ((block_idx+1) << 16) | entry_slot.
				 */
				ctx->pos = ((loff_t)(block_idx + 1) << 16)
					   | entry_slot;
				if (!dir_emit(ctx, de->d_name, de->d_name_len,
					      le64_to_cpu(de->d_ino),
					      de->d_file_type)) {
					brelse(bh);
					return 0;
				}
			}

			entry_slot++;
			offset += sizeof(struct beamfs_dir_entry);
		}
		brelse(bh);

		/* Reset start_slot for subsequent blocks */
		start_block = block_idx + 1;
		start_slot  = 0;
	}

	ctx->pos = INT_MAX;
	return 0;
}

/*
 * beamfs_lookup — find dentry in directory
 */
struct dentry *beamfs_lookup(struct inode *dir,
			    struct dentry *dentry,
			    unsigned int flags)
{
	struct super_block      *sb = dir->i_sb;
	struct beamfs_inode_info *fi = BEAMFS_I(dir);
	struct beamfs_dir_entry  *de;
	struct buffer_head      *bh;
	unsigned int  offset;
	unsigned long block_no;
	int i;

	if (dentry->d_name.len > BEAMFS_MAX_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		offset = 0;
		while (offset + sizeof(*de) <= BEAMFS_BLOCK_SIZE) {
			de = (struct beamfs_dir_entry *)(bh->b_data + offset);
			/*
			 * Skip free slots (d_ino == 0). lookup must traverse
			 * the entire block, not stop at the first hole, because
			 * the target entry may follow a deleted entry within
			 * the same block.
			 */
			if (de->d_ino &&
			    de->d_name_len == dentry->d_name.len &&
			    !memcmp(de->d_name, dentry->d_name.name,
				    dentry->d_name.len)) {
				u64 ino = le64_to_cpu(de->d_ino);
				struct inode *inode;

				brelse(bh);
				inode = beamfs_iget(sb, ino);
				return d_splice_alias(inode, dentry);
			}
			offset += sizeof(struct beamfs_dir_entry);
		}
		brelse(bh);
	}
	return d_splice_alias(NULL, dentry);
}

const struct file_operations beamfs_dir_operations = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = beamfs_readdir,
};
