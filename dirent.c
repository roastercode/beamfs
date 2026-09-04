// SPDX-License-Identifier: GPL-2.0-only
//
// Author: Aurelien Desbrieres <aurelien@hackers.camp>

/*
 * Directory blocks: variable-length entries, and parity over them.
 *
 * Every other structure on this volume was covered and directory blocks
 * were not. They were written with memset and mark_buffer_dirty --
 * 4096 raw bytes, no checksum, no correction -- so an upset there was
 * silent and permanent: a flipped inode number points a name at the
 * wrong file, a flipped length walks the parser off the end of an
 * entry, and neither is detectable let alone correctable. That was the
 * one hole left in the chain.
 *
 * Parity is inline, sixteen RS codewords in the block, matching the
 * data path. The alternative -- a separate parity region, which is what
 * the indirect blocks use -- keeps 4096 usable bytes but makes that
 * region a single point of failure for every directory on the volume,
 * and leaves the filesystem with three different parity models instead
 * of two. ext4 puts its directory checksum inline for the same reason.
 *
 * Entries became variable length in the same change, because a
 * protected block has 3824 usable bytes rather than 4096 and the layout
 * moves either way. d_rec_len was already in the on-disk entry and
 * already written correctly; nothing read it, because every walk
 * stepped by sizeof(struct beamfs_dir_entry) -- a flat 268 bytes,
 * whatever the name's length. Fourteen entries per block, and a hard
 * ceiling of 7336 per directory.
 *
 * With the field used, an eleven-character name takes 24 bytes: 144
 * entries per block and a ceiling above 75000. Eleven times the
 * density, eleven times fewer blocks read per lookup, and eleven times
 * less metadata exposed for the same directory.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "beamfs.h"

/*
 * Which subblock an offset falls in, and where that subblock ends.
 *
 * Each of the sixteen subblocks is an independent codeword: one is
 * correctable, or not, on its own. An entry spanning two turns a single
 * uncorrectable subblock into a parse failure for everything after it
 * in the block, so entries are kept inside one whenever they fit.
 */
static inline u32 beamfs_dirent_sub_end(u32 off)
{
	return (off / BEAMFS_SUBBLOCK_DATA + 1) * BEAMFS_SUBBLOCK_DATA;
}

/*
 * Where the next entry of @len bytes goes, given a cursor at @off.
 *
 * Returns the offset to place it at, which is @off unless the entry
 * would straddle a subblock boundary and does not have to. Returns
 * BEAMFS_DIRENT_NOSPACE when the block cannot hold it.
 *
 * An entry longer than a subblock -- a name past 226 characters, which
 * NAME_MAX allows and xfstests exercises -- is placed where it falls.
 * Refusing it would mean rejecting a filename every other Linux
 * filesystem accepts, and the property is kept everywhere it can be.
 */
u32 beamfs_dirent_place(u32 off, u16 len)
{
	u32 end;

	if (off + len > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;

	if (len > BEAMFS_SUBBLOCK_DATA)
		return off;			/* cannot fit either way */

	end = beamfs_dirent_sub_end(off);
	if (off + len <= end)
		return off;			/* fits where it is */

	if (end + len > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;

	return end;				/* start of the next subblock */
}

/*
 * Step to the entry after the one at @off.
 *
 * A record length of zero, or one that would leave the block, ends the
 * walk: on a damaged block that is the difference between stopping and
 * reading whatever follows as a directory entry.
 */
u32 beamfs_dirent_next(const struct beamfs_dir_entry *de, u32 off)
{
	u16 rec = le16_to_cpu(de->d_rec_len);

	if (rec < BEAMFS_DIRENT_MIN_LEN)
		return BEAMFS_DIRENT_NOSPACE;
	if (off + rec > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;
	return off + rec;
}

/*
 * Is the entry at @off structurally sound?
 *
 * Checked before the name is used, because a flipped length is exactly
 * the failure this parity was added to catch, and a name length that
 * runs past the record is how a directory read walks off into the next
 * entry.
 */
bool beamfs_dirent_valid(const struct beamfs_dir_entry *de, u32 off)
{
	u16 rec = le16_to_cpu(de->d_rec_len);

	if (rec < BEAMFS_DIRENT_MIN_LEN)
		return false;
	if (off + rec > BEAMFS_DATA_INLINE_BYTES)
		return false;
	if (de->d_name_len > BEAMFS_MAX_FILENAME)
		return false;
	if (BEAMFS_DIRENT_HDR_LEN + de->d_name_len > rec)
		return false;
	return true;
}

/*
 * Re-encode a directory block's parity. The buffer lock must be held.
 *
 * Same sixteen codewords as a data block, over the same 239-byte
 * subblocks, so a corrupted directory is correctable to the same depth
 * as corrupted file contents.
 */
void beamfs_dirent_encode(struct buffer_head *bh)
{
	u8 *b = (u8 *)bh->b_data;
	unsigned int i;

	/*
	 * The payload is laid out contiguously in the first 3824 bytes
	 * and scattered into the codeword slots here, the way the data
	 * path does it, so the walk above can treat the block as flat.
	 */
	for (i = BEAMFS_DATA_INLINE_SUBBLOCKS; i-- > 0; )
		memmove(b + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
			b + (size_t)i * BEAMFS_SUBBLOCK_DATA,
			BEAMFS_SUBBLOCK_DATA);

	beamfs_rs_encode_region(b, BEAMFS_SUBBLOCK_TOTAL,
				b + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS);
}

/*
 * Decode a directory block into @dst, correcting what RS can.
 *
 * @dst holds the 3824 payload bytes contiguously, which is what the
 * entry walk expects. Returns the number of subblocks that needed
 * correcting, or -EUCLEAN if any could not be.
 */
int beamfs_dirent_decode(struct super_block *sb, struct buffer_head *bh,
			 u8 *dst)
{
	int *results, *positions;
	unsigned int i, corrected = 0;
	int ret = 0;

	results = kcalloc(BEAMFS_DATA_INLINE_SUBBLOCKS, sizeof(*results),
			  GFP_NOFS);
	positions = kcalloc(BEAMFS_DATA_INLINE_SUBBLOCKS *
			    (BEAMFS_RS_PARITY / 2), sizeof(*positions),
			    GFP_NOFS);
	if (!results || !positions) {
		kfree(positions);
		kfree(results);
		return -ENOMEM;
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy(dst + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       (u8 *)bh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);

	ret = beamfs_rs_decode_region(dst, BEAMFS_SUBBLOCK_DATA,
				      (u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_SUBBLOCK_TOTAL,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, positions,
				      BEAMFS_RS_PARITY / 2,
				"directory");

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] > 0) {
			corrected++;
			beamfs_log_rs_event(sb,
				bh->b_blocknr * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA);
			beamfs_budget_record(sb, bh->b_blocknr,
					     (unsigned int)results[i]);
		} else if (results[i] < 0) {
			ret = -EUCLEAN;
		}
	}

	kfree(positions);
	kfree(results);
	return ret < 0 ? ret : (int)corrected;
}
