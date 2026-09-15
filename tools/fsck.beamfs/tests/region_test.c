// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * What a parity region survives, and what it does not.
 *
 * A region block carries sixteen RS codewords over its own contents,
 * and every slot in it belongs to a different indirect block. Editing
 * one slot therefore means decoding the whole region, changing a few
 * bytes, and encoding it again -- three operations where the middle
 * one is trivial and the other two are not.
 *
 * On 2026-09-15 a function doing exactly that was put on the free path
 * without a test. It produced 56 "parity region beyond correction"
 * messages in one sweep and took generic/269 from 218 leaked blocks to
 * 1898. The reasoning behind it was sound; the code was never run
 * against a region it had touched.
 *
 * These cases are what should have been asked first.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rs_decode.h"
#include "beamfs_format.h"

#define SUBS   BEAMFS_DATA_INLINE_SUBBLOCKS
#define DATA   BEAMFS_SUBBLOCK_DATA
#define PAR    BEAMFS_RS_PARITY
#define TOTAL  BEAMFS_SUBBLOCK_TOTAL

static int failures;
static int cases;

static void ok(const char *what, int pass)
{
	cases++;
	if (!pass) {
		failures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  pass  %s\n", what);
	}
}

/* Encode every subblock of a region, as the filesystem does. */
static void region_encode(struct rs_codec *rs, uint8_t *blk)
{
	unsigned int i;

	for (i = 0; i < SUBS; i++)
		rs_encode_subblock(rs, blk + (size_t)i * TOTAL, DATA,
				   blk + (size_t)i * TOTAL + DATA);
}

/* Decode it back; returns the number of subblocks that would not. */
static unsigned int region_decode(struct rs_codec *rs, uint8_t *blk)
{
	int positions[PAR / 2];
	unsigned int bad = 0;
	unsigned int i;

	for (i = 0; i < SUBS; i++) {
		int rc = rs_decode_subblock(rs, blk + (size_t)i * TOTAL, DATA,
					    blk + (size_t)i * TOTAL + DATA,
					    positions);
		if (rc == RS_UNCORRECTABLE)
			bad++;
	}
	return bad;
}

int main(void)
{
	struct rs_codec *rs = rs_init();
	static uint8_t blk[BEAMFS_BLOCK_SIZE];
	static uint8_t saved[BEAMFS_BLOCK_SIZE];
	unsigned int i;

	if (!rs) {
		fprintf(stderr, "region_test: no codec\n");
		return 77;
	}

	printf("parity region, read-modify-write\n");

	/*
	 * A region straight from the encoder decodes.
	 *
	 * The floor: if this fails nothing below it means anything.
	 */
	memset(blk, 0, sizeof(blk));
	for (i = 0; i < DATA; i++)
		blk[i] = (uint8_t)(i * 7 + 3);
	region_encode(rs, blk);
	ok("a freshly encoded region decodes", region_decode(rs, blk) == 0);

	/*
	 * One slot changed, then re-encoded, still decodes.
	 *
	 * This is what clearing a freed block's slot does, and it is the
	 * case that was never asked.
	 */
	memcpy(saved, blk, sizeof(blk));
	memset(blk + 64, 0, 16);		/* one slot, zeroed */
	region_encode(rs, blk);
	ok("a slot zeroed and re-encoded decodes",
	   region_decode(rs, blk) == 0);

	/*
	 * And the other slots kept their contents.
	 *
	 * An edit that decodes cleanly but rewrites its neighbours is
	 * worse than one that fails: it is the "correction" laundering
	 * one block's contents into another's that indparity.c warns
	 * about.
	 */
	ok("the other slots are untouched",
	   memcmp(blk, saved, 64) == 0 &&
	   memcmp(blk + 80, saved + 80, DATA - 80) == 0);

	/*
	 * Changed WITHOUT re-encoding: must not decode.
	 *
	 * The failure mode of forgetting the encode step. If this
	 * passes, the parity is not protecting anything.
	 */
	memcpy(blk, saved, sizeof(blk));
	memset(blk + 64, 0xff, 16);
	ok("a slot changed without re-encoding is caught",
	   region_decode(rs, blk) > 0);

	/*
	 * Eight symbols flipped in one subblock: corrected.
	 * Nine: not. That is what RS(255,239) promises and the whole
	 * design rests on it.
	 */
	memcpy(blk, saved, sizeof(blk));
	for (i = 0; i < 8; i++)
		blk[i] ^= 0xa5;
	ok("eight flipped symbols are corrected", region_decode(rs, blk) == 0);

	memcpy(blk, saved, sizeof(blk));
	for (i = 0; i < 12; i++)
		blk[i] ^= 0xa5;
	ok("twelve flipped symbols are refused", region_decode(rs, blk) > 0);

	/*
	 * A region of zeros.
	 *
	 * mkfs leaves it so, and every slot in it reads as "never
	 * written". It has to decode, or a fresh volume fails its first
	 * verify.
	 */
	memset(blk, 0, sizeof(blk));
	ok("an all-zero region decodes", region_decode(rs, blk) == 0);

	/*
	 * Encode twice, same answer.
	 *
	 * A non-deterministic encoder makes every comparison between
	 * two runs meaningless, and nothing else here would catch it.
	 */
	memset(blk, 0x5a, DATA);
	region_encode(rs, blk);
	memcpy(saved, blk, sizeof(blk));
	region_encode(rs, blk);
	ok("encoding is deterministic",
	   memcmp(blk, saved, sizeof(blk)) == 0);

	rs_free(rs);
	printf("\n%d case(s), %d failed\n", cases, failures);
	return failures ? 1 : 0;
}
