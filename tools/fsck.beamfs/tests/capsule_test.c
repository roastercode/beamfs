// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * A capsule, and what it survives.
 *
 * The case this exists for is the third one: a burst on the header.
 * Today the csum that says whether a block is sound and the selfid
 * that says whether it is the right one sit outside every codeword, so
 * nine bytes there condemn a block whose data is untouched. Inside the
 * coded area and interleaved, the same nine are one symbol in each of
 * sixteen codewords and the header comes back.
 *
 * A capsule that cannot protect its own header is not a unit of
 * survival, and this is where that claim is checked.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rs_decode.h"
#include "beamfs_format.h"

#define SUBS   BEAMFS_DATA_INLINE_SUBBLOCKS
#define CODED  BEAMFS_CAPSULE_CODED_BYTES
#define WORD   BEAMFS_SUBBLOCK_DATA
#define PAR    BEAMFS_RS_PARITY

static int failures, cases;
static uint8_t word[WORD];

static void ok(const char *what, int pass)
{
	cases++;
	printf("  %s  %s\n", pass ? "pass" : "FAIL", what);
	if (!pass)
		failures++;
}

/*
 * Symbol i of codeword j at byte i*16 + j, across the whole coded
 * area: data, csum and selfid alike. The header is not special to the
 * codec, which is the point.
 */
static void gather(const uint8_t *cap, unsigned int j)
{
	size_t i;

	for (i = 0; i < WORD; i++)
		word[i] = cap[i * SUBS + j];
}

static void scatter(uint8_t *cap, unsigned int j)
{
	size_t i;

	for (i = 0; i < WORD; i++)
		cap[i * SUBS + j] = word[i];
}

static void seal(struct rs_codec *rs, uint8_t *cap)
{
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		gather(cap, j);
		rs_encode_subblock(rs, word, WORD,
				   cap + BEAMFS_CAPSULE_PARITY_OFF
				       + (size_t)j * PAR);
	}
}

/* Returns the number of codewords that would not decode. */
static unsigned int open_it(struct rs_codec *rs, uint8_t *cap)
{
	int positions[PAR / 2];
	unsigned int bad = 0;
	unsigned int j;

	for (j = 0; j < SUBS; j++) {
		gather(cap, j);
		if (rs_decode_subblock(rs, word, WORD,
				       cap + BEAMFS_CAPSULE_PARITY_OFF
					   + (size_t)j * PAR,
				       positions) == RS_UNCORRECTABLE) {
			bad++;
			continue;
		}
		scatter(cap, j);
	}
	return bad;
}

int main(void)
{
	struct rs_codec *rs = rs_init();
	static uint8_t cap[BEAMFS_BLOCK_SIZE];
	static uint8_t src[CODED];
	size_t i;

	if (!rs) {
		fprintf(stderr, "capsule_test: no codec\n");
		return 77;
	}

	printf("a capsule, and what it survives\n");

	/* Data, then a header that means something. */
	for (i = 0; i < BEAMFS_CAPSULE_DATA_BYTES; i++)
		src[i] = (uint8_t)(i * 13 + 11);
	src[BEAMFS_CAPSULE_CSUM_OFF] = BEAMFS_CSUM_CRC32;
	for (i = 4; i < 8; i++)
		src[BEAMFS_CAPSULE_CSUM_OFF + i] = (uint8_t)(0xa0 + i);
	for (i = 0; i < 8; i++)
		src[BEAMFS_CAPSULE_SELFID_OFF + i] = (uint8_t)(0x50 + i);

	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	ok("a sealed capsule opens", open_it(rs, cap) == 0);

	/* A burst in the data. */
	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	for (i = 0; i < 9; i++)
		cap[500 + i] ^= 0xff;
	ok("nine bytes in the data are corrected", open_it(rs, cap) == 0);

	/*
	 * And the one that matters: a burst on the header.
	 *
	 * Outside the codewords this condemns the block. Inside them it
	 * is nine symbols spread over sixteen codewords.
	 */
	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	for (i = 0; i < 9; i++)
		cap[BEAMFS_CAPSULE_CSUM_OFF + i] ^= 0xff;
	ok("nine bytes on the header are corrected", open_it(rs, cap) == 0);
	ok("the csum and the selfid are what they were",
	   memcmp(cap + BEAMFS_CAPSULE_CSUM_OFF,
		  src + BEAMFS_CAPSULE_CSUM_OFF, 16) == 0);

	/* Straddling data and header, which is what a burst does. */
	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	for (i = 0; i < 24; i++)
		cap[BEAMFS_CAPSULE_CSUM_OFF - 12 + i] ^= 0x5a;
	ok("a burst across the data-header boundary is corrected",
	   open_it(rs, cap) == 0);
	ok("everything is what it was", memcmp(cap, src, CODED) == 0);

	/* The limit. */
	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	for (i = 0; i < 128; i++)
		cap[1000 + i] ^= 0x33;
	ok("a 128-byte burst is corrected", open_it(rs, cap) == 0);

	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	seal(rs, cap);
	for (i = 0; i < 160; i++)
		cap[1000 + i] ^= 0x33;
	ok("a 160-byte burst is refused", open_it(rs, cap) > 0);

	/*
	 * The generation lives outside the codewords, which is right: a
	 * stale one is caught by disagreeing with the tree, not by being
	 * decoded. What must hold is that encoding never touches it.
	 */
	memset(cap, 0, sizeof(cap));
	memcpy(cap, src, CODED);
	for (i = 0; i < 8; i++)
		cap[BEAMFS_CAPSULE_GEN_OFF + i] = (uint8_t)(0x70 + i);
	seal(rs, cap);
	{
		int kept = 1;

		for (i = 0; i < 8; i++)
			if (cap[BEAMFS_CAPSULE_GEN_OFF + i] != (uint8_t)(0x70 + i))
				kept = 0;
		ok("sealing leaves the generation alone", kept);
	}

	rs_free(rs);
	printf("\n%d case(s), %d failed\n", cases, failures);
	return failures ? 1 : 0;
}
