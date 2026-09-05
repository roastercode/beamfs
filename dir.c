// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Directory operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/fs_dirent.h>
#include "beamfs.h"

/*
 * beamfs_readdir - iterate directory entries
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
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8            *payload;
	unsigned long  block_no;
	u32            offset;
	int            block_idx;
	int            start_block;
	u32            start_off;
	int            ret = 0;

	if (ctx->pos == INT_MAX)
		return 0;

	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return 0;
	}

	/*
	 * The cookie carries a byte offset now, not a slot number.
	 *
	 * Slots existed because every entry was 268 bytes whatever its
	 * name; with variable-length entries there is no slot to count.
	 * Sixteen bits is more than the 3824 bytes a block holds, so the
	 * layout of the cookie is unchanged and telldir/seekdir keep
	 * working across the format change.
	 */
	if (ctx->pos <= 2) {
		start_block = 0;
		start_off   = 0;
	} else {
		start_block = (int)((ctx->pos >> 16) & 0x7FFF) - 1;
		start_off   = (u32)(ctx->pos & 0xFFFF);
	}

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return -ENOMEM;

	for (block_idx = start_block;
	     block_idx < (int)(BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS);
	     block_idx++) {
		u64 _bno;
		int _ret = beamfs_dir_get_block(inode, block_idx, false, &_bno);

		if (_ret) {
			ret = _ret;
			goto out;
		}
		block_no = (unsigned long)_bno;
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		/*
		 * Decode before reading anything. A flipped inode number
		 * points a name at the wrong file and a flipped length
		 * walks the parser into the next entry; both were silent
		 * before this block carried parity.
		 */
		_ret = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (_ret < 0) {
			pr_err_ratelimited("beamfs: directory block %lu uncorrectable\n",
					   block_no);
			ret = _ret;
			goto out;
		}

		offset = (block_idx == start_block) ? start_off : 0;

		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.' &&
			      de->d_name[1] == '.')) {
				u8 dt;

				ctx->pos = ((loff_t)(block_idx + 1) << 16)
					   | offset;

				switch (de->d_file_type) {
				case 1:
					dt = DT_REG;
					break;
				case 2:
					dt = DT_DIR;
					break;
				default:
					dt = de->d_file_type;
					break;
				}

				if (!dir_emit(ctx, de->d_name, de->d_name_len,
					      le64_to_cpu(de->d_ino), dt))
					goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}

		start_block = block_idx + 1;
		start_off   = 0;
	}

	ctx->pos = INT_MAX;
out:
	beamfs_scratch_put(sb, payload);
	return ret;
}

/*
 * beamfs_lookup - find dentry in directory
 */
struct dentry *beamfs_lookup(struct inode *dir,
			     struct dentry *dentry,
			     unsigned int flags)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	struct inode            *inode = NULL;
	u8            *payload;
	unsigned long  block_no;
	u32            offset;
	unsigned int   i;
	int            ret = 0;

	if (dentry->d_name.len > BEAMFS_MAX_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		u64 _bno;
		int _ret = beamfs_dir_get_block(dir, i, false, &_bno);

		if (_ret) {
			ret = _ret;
			goto out;
		}
		block_no = (unsigned long)_bno;
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		_ret = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (_ret < 0) {
			ret = _ret;
			goto out;
		}

		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    de->d_name_len == dentry->d_name.len &&
			    !memcmp(de->d_name, dentry->d_name.name,
				    de->d_name_len)) {
				inode = beamfs_iget(sb,
					(unsigned long)le64_to_cpu(de->d_ino));
				goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}
	}

out:
	beamfs_scratch_put(sb, payload);
	if (ret)
		return ERR_PTR(ret);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

const struct file_operations beamfs_dir_operations = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = beamfs_readdir,
};
