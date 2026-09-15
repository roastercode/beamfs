// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck_read -- one way in from the medium. See fsck_read.h.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsck_read.h"
#include "crc32.h"

/* Read exactly @len bytes at @off, or say the read failed. */
static bool read_exact(int fd, off_t off, void *buf, size_t len)
{
	if (lseek(fd, off, SEEK_SET) < 0)
		return false;

	/*
	 * Loop rather than trust one read(): a short read on a block
	 * device is rare and legal, and the old code treated it as an
	 * error on some paths and as success on others -- a partial
	 * inode read that returned early left the tail of the previous
	 * inode in the buffer and was walked as if it were real.
	 */
	uint8_t *p = buf;
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, p + got, len - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		got += (size_t)n;
	}
	return true;
}

const char *fsck_read_strerror(enum fsck_read_status s)
{
	switch (s) {
	case FSCK_READ_CLEAN:         return "clean";
	case FSCK_READ_CORRECTED:     return "corrected";
	case FSCK_READ_UNCORRECTABLE: return "uncorrectable";
	case FSCK_READ_UNDESCRIBED:  return "never described";
	case FSCK_READ_IO:            return "I/O error";
	}
	return "unknown";
}

enum fsck_read_status fsck_reader_open(struct fsck_reader *r, int fd)
{
	struct beamfs_super_block sb;
	enum fsck_read_status st = FSCK_READ_CLEAN;
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->fd = fd;
	r->interleaved = 0;
	r->rs = rs_init();
	if (!r->rs)
		return FSCK_READ_IO;

	if (!read_exact(fd, 0, &sb, sizeof(sb))) {
		rs_free(r->rs);
		r->rs = NULL;
		return FSCK_READ_IO;
	}

	/*
	 * Correct the superblock before reading geometry out of it.
	 *
	 * Every offset the checker computes comes from these fields. A
	 * flipped bit in s_data_start_blk moves the whole data region
	 * and makes every later pass report nonsense, with numbers that
	 * all look plausible.
	 *
	 * The superblock is not laid out like a data block, and a first
	 * attempt here assumed it was: it has its own coverage range
	 * that excludes s_crc32 and s_uuid, thirteen subblocks rather
	 * than sixteen, and its parity inside s_pad. Decoding it as a
	 * data block rejected every healthy volume -- pass 1 called the
	 * superblock OK and this called it unreadable, on the same
	 * bytes. The staging dance below is pass 1's, verbatim, because
	 * two implementations of one format is how pass 3 and pass 4
	 * came to disagree in the first place.
	 */
	{
		uint8_t staging[BEAMFS_SB_RS_STAGING_BYTES];
		uint8_t *parity = (uint8_t *)&sb + BEAMFS_SB_RS_PARITY_OFFSET;
		const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
		const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
		const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);
		unsigned int corrected = 0;

		memcpy(staging, &sb, off_crc32);
		memcpy(staging + off_crc32, (uint8_t *)&sb + off_uuid, off_pad - off_uuid);
		memset(staging + BEAMFS_SB_RS_COVERAGE_BYTES, 0,
		       sizeof(staging) - BEAMFS_SB_RS_COVERAGE_BYTES);

		for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
			int positions[BEAMFS_RS_PARITY / 2];
			int rc = rs_decode_subblock(r->rs,
						    staging + i * BEAMFS_SB_RS_DATA_LEN,
						    BEAMFS_SB_RS_DATA_LEN,
						    parity + i * BEAMFS_RS_PARITY,
						    positions);

			if (rc == RS_UNCORRECTABLE) {
				rs_free(r->rs);
				r->rs = NULL;
				return FSCK_READ_UNCORRECTABLE;
			}
			if (rc > 0)
				corrected += (unsigned int)rc;
		}

		if (corrected > 0) {
			memcpy(&sb, staging, off_crc32);
			memcpy((uint8_t *)&sb + off_uuid, staging + off_crc32,
			       off_pad - off_uuid);
			r->corrected++;
			st = FSCK_READ_CORRECTED;
		}
	}

	/*
	 * Which layout, read from the volume rather than passed in.
	 *
	 * Three passes open a reader and none of them has any reason to
	 * know about block layouts; the reader is already reading the
	 * superblock for the geometry, and this is one more field of it.
	 */
	r->interleaved = (sb.s_feat_incompat &
			  BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) ? 1 : 0;

	r->data_start       = sb.s_data_start_blk;
	r->nblocks          = sb.s_block_count > sb.s_data_start_blk
			    ? sb.s_block_count - sb.s_data_start_blk : 0;
	r->inode_table_blk  = sb.s_inode_table_blk;
	r->inode_count      = sb.s_inode_count;
	r->inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	r->ind_parity_blk   = sb.s_ind_parity_blk;
	r->ind_parity_len   = sb.s_ind_parity_len;
	r->ind_parity_mode  = sb.s_ind_parity_mode;

	if (r->inodes_per_block == 0 || r->inode_count == 0 || r->nblocks == 0) {
		rs_free(r->rs);
		r->rs = NULL;
		return FSCK_READ_UNCORRECTABLE;
	}
	return st;
}

void fsck_reader_close(struct fsck_reader *r)
{
	if (r->rs) {
		rs_free(r->rs);
		r->rs = NULL;
	}
}

enum fsck_read_status fsck_read_inode(struct fsck_reader *r, uint64_t ino,
				      struct beamfs_inode *out)
{
	uint64_t block, offset;
	off_t pos;
	int positions[BEAMFS_RS_PARITY / 2];
	int rc;

	if (ino == 0 || ino > r->inode_count)
		return FSCK_READ_IO;

	block  = r->inode_table_blk + (ino - 1) / r->inodes_per_block;
	offset = (ino - 1) % r->inodes_per_block;
	pos    = (off_t)block * BEAMFS_BLOCK_SIZE
	       + (off_t)offset * sizeof(struct beamfs_inode);

	if (!read_exact(r->fd, pos, out, sizeof(*out)))
		return FSCK_READ_IO;

	/* A free slot has no CRC worth checking. */
	if (out->i_mode == 0)
		return FSCK_READ_CLEAN;

	if (crc32_inode(out) == out->i_crc32)
		return FSCK_READ_CLEAN;

	rc = rs_decode_subblock(r->rs, (uint8_t *)out, BEAMFS_INODE_RS_DATA,
				out->i_reserved, positions);
	if (rc == RS_UNCORRECTABLE ||
	    crc32_inode(out) != out->i_crc32) {
		r->uncorrectable++;
		return FSCK_READ_UNCORRECTABLE;
	}
	r->corrected++;
	return FSCK_READ_CORRECTED;
}

/*
 * Where an indirect block's parity lives.
 *
 * Mirrors ind_parity_slot in the kernel. Kept here rather than shared
 * because the checker must work on a device the running kernel has
 * never seen, but the arithmetic has to match exactly -- a checker that
 * looks in the wrong place reports every indirect block as damaged.
 */
static bool ind_parity_slot(const struct fsck_reader *r, uint64_t phys,
			    uint64_t *region_blk, uint32_t *offset, size_t *stride)
{
	uint64_t index;
	unsigned int slots;

	if (r->ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		*stride = BEAMFS_IND_PARITY_CRC_BYTES;
		slots   = BEAMFS_IND_PARITY_CRC_SLOTS;
	} else {
		*stride = BEAMFS_IND_PARITY_RS_BYTES;
		slots   = BEAMFS_IND_PARITY_RS_SLOTS;
	}

	if (*stride == 0 || slots == 0 ||
	    r->ind_parity_blk == 0 || r->ind_parity_len == 0)
		return false;
	if (phys < r->data_start)
		return false;

	/*
	 * The offset is into the region block's payload, not its raw
	 * bytes. The block carries its own RS FEC now, so only
	 * BEAMFS_DATA_INLINE_BYTES of its 4096 are payload and fourteen
	 * RS slots fit where sixteen did.
	 */
	index       = phys - r->data_start;
	*region_blk = r->ind_parity_blk + index / slots;
	*offset     = (uint32_t)(index % slots) * (uint32_t)*stride;

	if (*offset + *stride > BEAMFS_DATA_INLINE_BYTES)
		return false;
	if (*region_blk >= r->ind_parity_blk + r->ind_parity_len)
		return false;
	return true;
}

/*
 * Decode a region block into @flat.
 *
 * Returns false when a subblock is beyond correction: the region was
 * hit, not the indirect block it describes, and saying the block is
 * damaged would blame the wrong one. Before the region carried its own
 * FEC there was no way to tell them apart.
 */
static bool ind_region_decode(struct fsck_reader *r, uint8_t *raw,
			      uint8_t *flat)
{
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = raw + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		int positions[BEAMFS_RS_PARITY / 2];
		int rc = rs_decode_subblock(r->rs, sub, BEAMFS_SUBBLOCK_DATA,
					    sub + BEAMFS_SUBBLOCK_DATA,
					    positions);

		if (rc == RS_UNCORRECTABLE)
			return false;
		if (rc > 0)
			r->corrected++;
		memcpy(flat + (size_t)i * BEAMFS_SUBBLOCK_DATA, sub,
		       BEAMFS_SUBBLOCK_DATA);
	}
	return true;
}

enum fsck_read_status fsck_read_indirect(struct fsck_reader *r, uint64_t blk,
					 uint64_t *out)
{
	uint8_t raw[BEAMFS_BLOCK_SIZE];
	uint8_t parity[BEAMFS_BLOCK_SIZE];
	uint8_t flat[BEAMFS_DATA_INLINE_BYTES];
	uint64_t region_blk;
	uint32_t offset;
	size_t stride;
	unsigned int i;
	enum fsck_read_status st = FSCK_READ_CLEAN;

	/* Same as above: an indirect block below data_start is unusual
	 * but not impossible, and the device's end is the real bound.
	 */
	if (blk == 0 || blk >= r->data_start + r->nblocks)
		return FSCK_READ_IO;

	if (!read_exact(r->fd, (off_t)blk * BEAMFS_BLOCK_SIZE, raw, sizeof(raw)))
		return FSCK_READ_IO;

	/*
	 * Verify against the parity region when the volume has one.
	 *
	 * A v4 volume has none, and refusing to read it would make the
	 * checker useless on exactly the images kept from earlier
	 * campaigns. Absence of parity is not damage.
	 */
	if (ind_parity_slot(r, blk, &region_blk, &offset, &stride) &&
	    read_exact(r->fd, (off_t)region_blk * BEAMFS_BLOCK_SIZE, parity, sizeof(parity)) &&
	    ind_region_decode(r, parity, flat)) {
		size_t k;
		int described = 0;

		/*
		 * An empty slot is not a failed check.
		 *
		 * Every block written through the kernel gets its parity
		 * written in the same breath, so a slot of zeros means no
		 * block was ever described here. Running the decoder
		 * against zeros reports "beyond correction", which points
		 * at the medium when the block was simply never written.
		 */
		for (k = 0; k < stride; k++) {
			if (flat[offset + k]) {
				described = 1;
				break;
			}
		}
		if (!described) {
			r->undescribed++;
			return FSCK_READ_UNDESCRIBED;
		}

		if (r->ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
			const uint32_t *slot = (const uint32_t *)(flat + offset);

			for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
				uint32_t got = crc32(raw + (size_t)i * BEAMFS_SUBBLOCK_DATA,
						     BEAMFS_SUBBLOCK_DATA);
				if (got != slot[i]) {
					/* CRC detects; it cannot correct.
					 * The pointers are not to be
					 * followed.
					 */
					r->uncorrectable++;
					return FSCK_READ_UNCORRECTABLE;
				}
			}
		} else {
			/*
			 * Decoded in a copy, never in raw.
			 *
			 * rs_decode_subblock corrects in place, and a
			 * subblock past its correction radius has already
			 * written the decoder's guesses into the buffer
			 * before it gives up. Decoding straight into raw
			 * meant an uncorrectable block was destroyed on the
			 * way to being declared uncorrectable, and the
			 * caller read the wreckage.
			 *
			 * generic/464 shows what that cost: block 20084
			 * holds four valid pointers on the medium and came
			 * back with 366, among them 17179869188 and
			 * 1999944 -- decoder artefacts, not data. Three
			 * different blocks came back with nearly the same
			 * numbers. Every count of lost blocks on a volume
			 * with an uncorrectable indirect was taken from
			 * that.
			 *
			 * The kernel already decodes into scratch for the
			 * same reason; beamfs_ind_parity_verify records the
			 * 500-block leak that taught it.
			 */
			uint8_t work[BEAMFS_BLOCK_SIZE];
			int failed = 0;

			memcpy(work, raw, sizeof(work));
			for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
				int positions[BEAMFS_RS_PARITY / 2];
				int rc = rs_decode_subblock(
					r->rs,
					work + (size_t)i * BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_DATA,
					flat + offset + (size_t)i * BEAMFS_RS_PARITY,
					positions);

				if (rc == RS_UNCORRECTABLE) {
					failed = 1;
					break;
				}
				if (rc > 0)
					st = FSCK_READ_CORRECTED;
			}
			if (failed) {
				r->uncorrectable++;
				return FSCK_READ_UNCORRECTABLE;
			}
			/* Every subblock decoded: the copy is the truth. */
			memcpy(raw, work, sizeof(work));
			if (st == FSCK_READ_CORRECTED)
				r->corrected++;
		}
	}

	/* Byte-swapped here so no caller can forget to. */
	for (i = 0; i < BEAMFS_INDIRECT_PTRS; i++) {
		uint64_t le;

		memcpy(&le, raw + (size_t)i * sizeof(uint64_t), sizeof(le));
		out[i] = le64toh(le);
	}
	return st;
}

enum fsck_read_status fsck_read_data(struct fsck_reader *r, uint64_t blk,
				     uint8_t *out)
{
	uint8_t raw[BEAMFS_BLOCK_SIZE];
	unsigned int i;
	enum fsck_read_status st = FSCK_READ_CLEAN;

	/*
	 * Below data_start is still a real block.
	 *
	 * mkfs puts the root directory and the canary just under
	 * data_start, outside the allocation bitmap: they are never
	 * allocated and never freed, which is why pass 4 neither counts
	 * nor misses them. Refusing to read them made pass 6 see an
	 * empty buffer where the root's entries are, report a record
	 * length of zero, and call a pristine volume malformed.
	 *
	 * What must be rejected is a block outside the device.
	 */
	if (blk == 0 || blk >= r->data_start + r->nblocks)
		return FSCK_READ_IO;

	if (!read_exact(r->fd, (off_t)blk * BEAMFS_BLOCK_SIZE, raw, sizeof(raw)))
		return FSCK_READ_IO;

	if (r->interleaved) {
		/*
		 * A capsule: gather each codeword from every sixteenth
		 * byte, decode it, and put it back. The data comes out
		 * where it already is -- interleaving moves symbols
		 * within a codeword, not the bytes a reader sees.
		 */
		static uint8_t word[BEAMFS_SUBBLOCK_DATA];

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			int positions[BEAMFS_RS_PARITY / 2];
			size_t k;
			int rc;

			for (k = 0; k < BEAMFS_SUBBLOCK_DATA; k++)
				word[k] = raw[k * BEAMFS_DATA_INLINE_SUBBLOCKS + i];

			rc = rs_decode_subblock(r->rs, word,
						BEAMFS_SUBBLOCK_DATA,
						raw + BEAMFS_CAPSULE_PARITY_OFF
						    + (size_t)i * BEAMFS_RS_PARITY,
						positions);
			if (rc == RS_UNCORRECTABLE) {
				r->uncorrectable++;
				return FSCK_READ_UNCORRECTABLE;
			}
			if (rc > 0)
				st = FSCK_READ_CORRECTED;

			for (k = 0; k < BEAMFS_SUBBLOCK_DATA; k++)
				raw[k * BEAMFS_DATA_INLINE_SUBBLOCKS + i] = word[k];
		}
		memcpy(out, raw, BEAMFS_CAPSULE_DATA_BYTES);
		if (st == FSCK_READ_CORRECTED)
			r->corrected++;
		return st;
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		int positions[BEAMFS_RS_PARITY / 2];
		uint8_t *sub = raw + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		int rc = rs_decode_subblock(r->rs, sub, BEAMFS_SUBBLOCK_DATA,
					    sub + BEAMFS_SUBBLOCK_DATA, positions);

		if (rc == RS_UNCORRECTABLE) {
			r->uncorrectable++;
			return FSCK_READ_UNCORRECTABLE;
		}
		if (rc > 0)
			st = FSCK_READ_CORRECTED;

		/* Flat on the way out: a caller reading directory
		 * records has no business knowing where parity sits.
		 */
		memcpy(out + (size_t)i * BEAMFS_SUBBLOCK_DATA, sub,
		       BEAMFS_SUBBLOCK_DATA);
	}
	if (st == FSCK_READ_CORRECTED)
		r->corrected++;
	return st;
}

enum fsck_read_status fsck_read_bitmap(struct fsck_reader *r, uint32_t index,
				       uint8_t *out)
{
	uint8_t raw[BEAMFS_BLOCK_SIZE];
	struct beamfs_super_block sb;
	unsigned int i;
	enum fsck_read_status st = FSCK_READ_CLEAN;

	if (!read_exact(r->fd, 0, &sb, sizeof(sb)))
		return FSCK_READ_IO;
	if (!read_exact(r->fd, (off_t)(sb.s_bitmap_blk + index) * BEAMFS_BLOCK_SIZE,
			raw, sizeof(raw)))
		return FSCK_READ_IO;

	for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++) {
		int positions[BEAMFS_RS_PARITY / 2];
		uint8_t *sub = raw + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		int rc = rs_decode_subblock(r->rs, sub, BEAMFS_SUBBLOCK_DATA,
					    sub + BEAMFS_SUBBLOCK_DATA, positions);

		if (rc == RS_UNCORRECTABLE) {
			r->uncorrectable++;
			return FSCK_READ_UNCORRECTABLE;
		}
		if (rc > 0)
			st = FSCK_READ_CORRECTED;

		/* Contiguous on the way out: callers count bits and have
		 * no business knowing where the parity sits.
		 */
		memcpy(out + (size_t)i * BEAMFS_SUBBLOCK_DATA, sub,
		       BEAMFS_SUBBLOCK_DATA);
	}
	if (st == FSCK_READ_CORRECTED)
		r->corrected++;
	return st;
}
