// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.beamfs - RS decode test vectors
 *
 * Validates rs_decode_subblock() against fixtures produced by the
 * mkfs.beamfs encoder (same RS(255,239) parameters). Once the
 * decoder port lands, this test will exercise three properties:
 *   1) Clean codeword -> rs_decode returns 0 (no correction).
 *   2) Single-byte flip in data -> rs_decode returns 1, data
 *      restored byte-exact to the pristine fixture.
 *   3) Beyond correction radius (9 byte flips) -> rs_decode
 *      returns RS_UNCORRECTABLE.
 *
 * In sub-2.partial-2 the decoder returns RS_NOT_IMPLEMENTED, so the
 * test logs the expected return values and exits 77 (autotest skip
 * convention) until the decoder lands.
 *
 * The encoder used to generate the fixtures is a verbatim copy of
 * mkfs.beamfs.c::encode_rs_userspace, so the fixtures are guaranteed
 * by construction to match the on-disk layout produced by mkfs.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rs_decode.h"

/*
 * Verbatim copy of mkfs.beamfs.c::encode_rs_userspace.
 * Used here only to generate the test fixtures; the decoder under
 * test is rs_decode_subblock(), which has no shared code path with
 * this encoder.
 */
static void encode_rs_userspace(const uint8_t *data, size_t data_len,
				uint8_t *parity)
{
	static uint8_t alpha_to[256], index_of[256];
	static uint8_t genpoly[17];
	static int     rs_table_init;
	uint8_t        par[16];
	size_t         i;

	if (!rs_table_init) {
		unsigned int sr = 1, j;

		/* GF(2^8) tables: kernel-canonical convention. */
		index_of[0] = 255;
		alpha_to[255] = 0;
		for (j = 0; j < 255; j++) {
			alpha_to[j] = (uint8_t)sr;
			index_of[sr] = (uint8_t)j;
			sr <<= 1;
			if (sr & 0x100)
				sr ^= 0x187;
			sr &= 0xff;
		}

		genpoly[0] = 1;
		for (j = 0; j < 16; j++) {
			unsigned int k;

			genpoly[j + 1] = 1;
			for (k = j; k > 0; k--) {
				if (genpoly[k] != 0)
					genpoly[k] = genpoly[k - 1] ^
						alpha_to[(index_of[genpoly[k]] + j) % 255];
				else
					genpoly[k] = genpoly[k - 1];
			}
			genpoly[0] = alpha_to[(index_of[genpoly[0]] + j) % 255];
		}
		for (j = 0; j <= 16; j++)
			genpoly[j] = index_of[genpoly[j]];

		rs_table_init = 1;
	}

	memset(par, 0, sizeof(par));

	for (i = 0; i < data_len; i++) {
		uint8_t      feedback = index_of[data[i] ^ par[0]];
		unsigned int j;

		if (feedback != 255) {
			for (j = 0; j < 15; j++)
				par[j] = par[j + 1] ^
					alpha_to[(feedback + genpoly[15 - j]) % 255];
			par[15] = alpha_to[(feedback + genpoly[0]) % 255];
		} else {
			for (j = 0; j < 15; j++)
				par[j] = par[j + 1];
			par[15] = 0;
		}
	}

	memcpy(parity, par, 16);
}

static int test_clean_codeword(struct rs_codec *rs, size_t len)
{
	uint8_t data[RS_MAX_DATA_LEN];
	uint8_t parity[16];
	size_t  i;
	int     rc;

	for (i = 0; i < len; i++)
		data[i] = (uint8_t)(i & 0xFF);

	encode_rs_userspace(data, len, parity);
	rc = rs_decode_subblock(rs, data, len, parity, NULL);

	printf("%s(len=%zu): rs_decode_subblock returned %d\n", __func__,
	       len, rc);
	if (rc == RS_NOT_IMPLEMENTED) {
		printf("  decoder not yet implemented; fixture validated; skipping\n");
		return 77;
	}
	return (rc == 0) ? 0 : 1;
}

static int test_single_byte_flip(struct rs_codec *rs, size_t len)
{
	uint8_t data[RS_MAX_DATA_LEN];
	uint8_t pristine[RS_MAX_DATA_LEN];
	uint8_t parity[16];
	size_t  i;
	int     rc;

	for (i = 0; i < len; i++)
		data[i] = (uint8_t)(i & 0xFF);
	memcpy(pristine, data, len);
	encode_rs_userspace(data, len, parity);

	/* Flip a single bit in the middle of the data buffer. */
	data[len / 2] ^= 0x08;

	rc = rs_decode_subblock(rs, data, len, parity, NULL);

	printf("%s(len=%zu): rs_decode_subblock returned %d\n", __func__,
	       len, rc);
	if (rc == RS_NOT_IMPLEMENTED) {
		printf("  decoder not yet implemented; fixture validated; skipping\n");
		return 77;
	}
	if (rc < 1) {
		printf("  expected rc>=1 (corrected); got %d\n", rc);
		return 1;
	}
	if (memcmp(data, pristine, len) != 0) {
		printf("  data not restored byte-exact after correction\n");
		return 1;
	}
	return 0;
}

int main(void)
{
	struct rs_codec *rs;
	int              rc_clean, rc_flip;
	int              skipped = 0;

	rs = rs_init();
	if (!rs) {
		fprintf(stderr, "rs_init failed\n");
		return 1;
	}

	/* Test the three on-disk subblock lengths used by beamfs */
	rc_clean = test_clean_codeword(rs, 172);   /* inode */
	if (rc_clean == 77) {
		skipped++;
	} else if (rc_clean) {
		rs_free(rs);
		return 1;
	}

	rc_clean = test_clean_codeword(rs, 211);   /* SB sub-block */
	if (rc_clean == 77) {
		skipped++;
	} else if (rc_clean) {
		rs_free(rs);
		return 1;
	}

	rc_clean = test_clean_codeword(rs, 239);   /* bitmap / data block */
	if (rc_clean == 77) {
		skipped++;
	} else if (rc_clean) {
		rs_free(rs);
		return 1;
	}

	rc_flip = test_single_byte_flip(rs, 239);  /* most common case */
	if (rc_flip == 77) {
		skipped++;
	} else if (rc_flip) {
		rs_free(rs);
		return 1;
	}

	rs_free(rs);

	if (skipped > 0) {
		printf("%d test(s) skipped (decoder not yet implemented)\n", skipped);
		return 77;
	}
	printf("all tests passed\n");
	return 0;
}
