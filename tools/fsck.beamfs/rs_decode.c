// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.beamfs - Reed-Solomon decoder (userland port)
 *
 * Port of lib/reed_solomon/reed_solomon.c (init_rs / codec_init)
 * from linux kernel.org, GPL-2.0. Adaptations:
 *   - kmalloc/kzalloc/kfree   -> malloc/calloc/free
 *   - BUG_ON                  -> assert
 *   - list_head / codec_list  -> dropped (single codec per run)
 *   - gffunc non-canonical    -> dropped (we only need gfpoly path)
 *   - rs_modnn(rs, x)         -> static inline (x % rs->nn)
 *
 * Codec parameters (kernel beamfs match):
 *   symsize=8, gfpoly=0x187, fcr=0, prim=1, nroots=16
 *
 * rs_decode_subblock() body lands in the next commit (BMA + Chien
 * search port from lib/reed_solomon/decode_rs.c). Until then it
 * returns RS_NOT_IMPLEMENTED; the test vector fixture confirms the
 * codec tables (alpha_to / index_of / genpoly) match the mkfs.beamfs
 * encoder byte-exactly via test_codec_tables.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rs_decode.h"
#include "rs_decode_internal.h"

static inline uint16_t rs_modnn(const struct rs_codec *rs, uint32_t x)
{
	return (uint16_t)(x % (uint32_t)rs->nn);
}

struct rs_codec *rs_init(void)
{
	struct rs_codec *rs;
	const int        symsize = RS_SYMSIZE;
	const int        gfpoly  = RS_GFPOLY;
	const int        fcr     = RS_FCR;
	const int        prim    = RS_PRIM;
	const int        nroots  = RS_NROOTS;
	int              i, j, sr, root, iprim;

	rs = calloc(1, sizeof(*rs));
	if (!rs)
		return NULL;

	rs->mm     = symsize;
	rs->nn     = (1 << symsize) - 1;
	rs->fcr    = fcr;
	rs->prim   = prim;
	rs->nroots = nroots;
	rs->gfpoly = gfpoly;

	/* Generate Galois field lookup tables (gfpoly path only). */
	rs->index_of[0]      = (uint16_t)rs->nn; /* log(zero) = -inf */
	rs->alpha_to[rs->nn] = 0;                /* alpha**-inf = 0 */

	sr = 1;
	for (i = 0; i < rs->nn; i++) {
		rs->index_of[sr] = (uint16_t)i;
		rs->alpha_to[i]  = (uint16_t)sr;
		sr <<= 1;
		if (sr & (1 << symsize))
			sr ^= gfpoly;
		sr &= rs->nn;
	}

	/* Verify primitivity: sr should have wrapped to alpha_to[0]. */
	if (sr != rs->alpha_to[0]) {
		free(rs);
		return NULL;
	}

	/* Find prim-th root of 1, used in decoding. */
	for (iprim = 1; (iprim % prim) != 0; iprim += rs->nn)
		;
	rs->iprim = iprim / prim;

	/* Form RS code generator polynomial from its roots. */
	rs->genpoly[0] = 1;
	for (i = 0, root = fcr * prim; i < nroots; i++, root += prim) {
		rs->genpoly[i + 1] = 1;
		/* Multiply rs->genpoly[] by  @**(root + x) */
		for (j = i; j > 0; j--) {
			if (rs->genpoly[j] != 0)
				rs->genpoly[j] = rs->genpoly[j - 1] ^
					rs->alpha_to[rs_modnn(rs,
						rs->index_of[rs->genpoly[j]] + root)];
			else
				rs->genpoly[j] = rs->genpoly[j - 1];
		}
		/* rs->genpoly[0] can never be zero. */
		rs->genpoly[0] = rs->alpha_to[rs_modnn(rs,
			rs->index_of[rs->genpoly[0]] + root)];
	}

	/* Convert rs->genpoly[] to index form for quicker encoding. */
	for (i = 0; i <= nroots; i++)
		rs->genpoly[i] = rs->index_of[rs->genpoly[i]];

	return rs;
}

void rs_free(struct rs_codec *rs)
{
	free(rs);
}

int rs_decode_subblock(struct rs_codec *rs,
		       uint8_t *data, size_t len,
		       uint8_t *parity,
		       int *positions)
{
	(void)rs;
	(void)data;
	(void)len;
	(void)parity;
	(void)positions;
	return RS_NOT_IMPLEMENTED;
}
