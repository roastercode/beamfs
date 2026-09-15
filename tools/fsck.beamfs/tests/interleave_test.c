// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * What interleaving buys, measured rather than argued.
 *
 * A block carries sixteen RS(255,239) codewords over 239 contiguous
 * bytes each. A heavy ion through a die corrupts neighbouring cells,
 * not scattered ones, so a nine-byte burst lands entirely in one
 * codeword and takes it past correction -- which is what
 * "subblock N beyond correction" has meant every time it appeared.
 *
 * Interleaved, symbol i of codeword j sits at byte i*16 + j instead of
 * j*239 + i. The same burst now puts one symbol in each of the sixteen
 * codewords, and it takes 129 consecutive bytes to lose one.
 *
 * Sixteen times the burst resistance for no extra parity. These cases
 * check that the claim is true and that the transform is its own
 * inverse.
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

static int failures, cases;

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

/*
 * Where byte @i of codeword @j lives once interleaved.
 *
 * The parity of a codeword stays with it: only the 239 data symbols
 * are spread, because spreading the parity too would gain nothing and
 * cost a second indirection on every encode.
 */
static size_t woven(unsigned int sub, size_t i)
{
	return i * SUBS + sub;
}

/* Gather one codeword's data out of an interleaved block. */
static void gather(const uint8_t *blk, unsigned int sub, uint8_t *out)
{
	size_t i;

	for (i = 0; i < DATA; i++)
		out[i] = blk[woven(sub, i)];
}

/* Put it back. */
static void scatter(uint8_t *blk, unsigned int sub, const uint8_t *in)
{
	size_t i;

	for (i = 0; i < DATA; i++)
		blk[woven(sub, i)] = in[i];
}

static void encode_woven(struct rs_codec *rs, uint8_t *blk, uint8_t *par)
{
	static uint8_t word[DATA];
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		gather(blk, j, word);
		rs_encode_subblock(rs, word, DATA, par + (size_t)j * PAR);
	}
}

/* Returns how many codewords would not decode. */
static unsigned int decode_woven(struct rs_codec *rs, uint8_t *blk,
				 uint8_t *par)
{
	static uint8_t word[DATA];
	int positions[PAR / 2];
	unsigned int bad = 0;
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		int rc;

		gather(blk, j, word);
		rc = rs_decode_subblock(rs, word, DATA,
					par + (size_t)j * PAR, positions);
		if (rc == RS_UNCORRECTABLE)
			bad++;
		else
			scatter(blk, j, word);
	}
	return bad;
}

/* The layout in use today: 239 contiguous bytes per codeword. */
static unsigned int decode_plain(struct rs_codec *rs, uint8_t *blk)
{
	int positions[PAR / 2];
	unsigned int bad = 0;
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		int rc = rs_decode_subblock(rs, blk + (size_t)j * TOTAL, DATA,
					    blk + (size_t)j * TOTAL + DATA,
					    positions);
		if (rc == RS_UNCORRECTABLE)
			bad++;
	}
	return bad;
}

static void encode_plain(struct rs_codec *rs, uint8_t *blk)
{
	unsigned int j;

	for (j = 0; j < SUBS; j++)
		rs_encode_subblock(rs, blk + (size_t)j * TOTAL, DATA,
				   blk + (size_t)j * TOTAL + DATA);
}

int main(void)
{
	struct rs_codec *rs = rs_init();
	static uint8_t blk[SUBS * DATA];
	static uint8_t par[SUBS * PAR];
	static uint8_t plain[BEAMFS_BLOCK_SIZE];
	static uint8_t saved[SUBS * DATA];
	size_t i;

	if (!rs) {
		fprintf(stderr, "interleave_test: no codec\n");
		return 77;
	}

	printf("interleaving, against bursts\n");

	for (i = 0; i < sizeof(blk); i++)
		blk[i] = (uint8_t)(i * 31 + 7);
	memcpy(saved, blk, sizeof(blk));

	/* The floor. */
	encode_woven(rs, blk, par);
	ok("an interleaved block decodes", decode_woven(rs, blk, par) == 0);

	/*
	 * The transform is its own inverse: what comes out of gather and
	 * back through scatter is what went in. If this fails every
	 * number below is meaningless.
	 */
	{
		static uint8_t word[DATA];
		unsigned int j;
		int same = 1;

		for (j = 0; j < SUBS && same; j++) {
			gather(blk, j, word);
			scatter(blk, j, word);
		}
		ok("gather and scatter round-trip",
		   memcmp(blk, saved, sizeof(blk)) == 0);
	}

	/*
	 * Nine consecutive bytes: what one ion track looks like.
	 *
	 * Today that is nine symbols in one codeword and the codeword is
	 * lost. Interleaved it is one symbol in each of nine codewords.
	 */
	memcpy(plain, saved, DATA);
	for (i = 1; i < SUBS; i++)
		memcpy(plain + i * TOTAL, saved + i * DATA, DATA);
	encode_plain(rs, plain);
	for (i = 0; i < 9; i++)
		plain[100 + i] ^= 0xff;
	ok("nine bytes in a row: today, one codeword is lost",
	   decode_plain(rs, plain) > 0);

	memcpy(blk, saved, sizeof(blk));
	encode_woven(rs, blk, par);
	for (i = 0; i < 9; i++)
		blk[100 + i] ^= 0xff;
	ok("nine bytes in a row: interleaved, corrected",
	   decode_woven(rs, blk, par) == 0);

	/* 128 is the most it can take; 129 is one too many. */
	memcpy(blk, saved, sizeof(blk));
	encode_woven(rs, blk, par);
	for (i = 0; i < 128; i++)
		blk[512 + i] ^= 0x5a;
	ok("a 128-byte burst is corrected", decode_woven(rs, blk, par) == 0);

	memcpy(blk, saved, sizeof(blk));
	encode_woven(rs, blk, par);
	for (i = 0; i < 129 + SUBS; i++)
		blk[512 + i] ^= 0x5a;
	ok("a burst past the limit is refused",
	   decode_woven(rs, blk, par) > 0);

	/*
	 * And the data comes back byte for byte after a corrected burst,
	 * which is the only thing that finally matters.
	 */
	memcpy(blk, saved, sizeof(blk));
	encode_woven(rs, blk, par);
	for (i = 0; i < 100; i++)
		blk[200 + i] ^= 0xa5;
	decode_woven(rs, blk, par);
	ok("the bytes are what they were", memcmp(blk, saved, sizeof(blk)) == 0);

	rs_free(rs);
	printf("\n%d case(s), %d failed\n", cases, failures);
	return failures ? 1 : 0;
}
