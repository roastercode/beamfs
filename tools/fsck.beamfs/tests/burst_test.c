// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * A nine-byte burst on a real block, and what it costs.
 *
 * This is the failing test. It builds a block exactly as the format
 * lays one out today -- sixteen subblocks of 239 data bytes each
 * followed by 16 of parity, alternating -- flips nine consecutive
 * bytes, and asks for them back.
 *
 * Today it cannot have them: the nine land in one codeword, which
 * corrects eight, and the subblock is lost. That is what a heavy ion
 * through a die produces and what "subblock N beyond correction" has
 * meant every time it has appeared in a sweep.
 *
 * It passes when the layout groups data and parity, so the symbols of
 * one codeword can be spread across the block and a burst of 139 is
 * needed to lose one. Until then it is expected to fail, and that is
 * the point.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rs_decode.h"
#include "beamfs_format.h"

#define SUBS   BEAMFS_DATA_INLINE_SUBBLOCKS
#define DATA   BEAMFS_SUBBLOCK_DATA
#define PAR    BEAMFS_RS_PARITY
#define TOTAL  BEAMFS_SUBBLOCK_TOTAL

static int failures, cases;

static void ok(const char *what, int pass)
{
	cases++;
	printf("  %s  %s\n", pass ? "pass" : "FAIL", what);
	if (!pass)
		failures++;
}

/* The block as the format lays it out today. */
static void build(struct rs_codec *rs, uint8_t *blk, const uint8_t *src)
{
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		memcpy(blk + (size_t)j * TOTAL, src + (size_t)j * DATA, DATA);
		rs_encode_subblock(rs, blk + (size_t)j * TOTAL, DATA,
				   blk + (size_t)j * TOTAL + DATA);
	}
}

/* Returns the number of subblocks that would not decode. */
static unsigned int repair(struct rs_codec *rs, uint8_t *blk)
{
	int positions[PAR / 2];
	unsigned int bad = 0;
	unsigned int j;

	for (j = 0; j < SUBS; j++)
		if (rs_decode_subblock(rs, blk + (size_t)j * TOTAL, DATA,
				       blk + (size_t)j * TOTAL + DATA,
				       positions) == RS_UNCORRECTABLE)
			bad++;
	return bad;
}

int main(void)
{
	struct rs_codec *rs = rs_init();
	static uint8_t src[SUBS * DATA];
	static uint8_t blk[BEAMFS_BLOCK_SIZE];
	static uint8_t got[SUBS * DATA];
	unsigned int j;
	size_t i;

	if (!rs) {
		fprintf(stderr, "burst_test: no codec\n");
		return 77;
	}

	printf("a burst on a block, as the format lays one out\n");

	for (i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 17 + 5);

	/* Eight bytes: within what one codeword corrects. */
	memset(blk, 0, sizeof(blk));
	build(rs, blk, src);
	for (i = 0; i < 8; i++)
		blk[600 + i] ^= 0xff;
	ok("eight consecutive bytes are corrected", repair(rs, blk) == 0);

	/*
	 * Nine. One more than a codeword can take, and they are all in
	 * the same one because a codeword owns 239 consecutive bytes.
	 */
	memset(blk, 0, sizeof(blk));
	build(rs, blk, src);
	for (i = 0; i < 9; i++)
		blk[600 + i] ^= 0xff;
	ok("nine consecutive bytes are corrected", repair(rs, blk) == 0);

	/* And the bytes come back. */
	for (j = 0; j < SUBS; j++)
		memcpy(got + (size_t)j * DATA, blk + (size_t)j * TOTAL, DATA);
	ok("the data is what it was", memcmp(got, src, sizeof(src)) == 0);

	/*
	 * A whole ion track: sixteen bytes, which is what a single event
	 * upset across a word line looks like.
	 */
	memset(blk, 0, sizeof(blk));
	build(rs, blk, src);
	for (i = 0; i < 16; i++)
		blk[600 + i] ^= 0xff;
	ok("sixteen consecutive bytes are corrected", repair(rs, blk) == 0);

	rs_free(rs);
	printf("\n%d case(s), %d failed\n", cases, failures);
	if (failures)
		printf("\nexpected until the layout groups data and parity:\n"
		       "a codeword owning 239 consecutive bytes cannot\n"
		       "survive a burst longer than eight.\n");
	return failures ? 1 : 0;
}
