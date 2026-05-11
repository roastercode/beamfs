// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.beamfs - Reed-Solomon decoder (userland skeleton)
 *
 * Sub-phase 2.partial-2: API + lifecycle skeleton, no decoder body.
 * The decoder body is a userland port of
 *   linux/lib/reed_solomon/reed_solomon.c (init_rs / codec_init)
 *   linux/lib/reed_solomon/decode_rs.c   (the BMA + Chien search)
 * adapted as follows:
 *   - kmalloc/kzalloc/kfree   -> malloc/calloc/free
 *   - BUG_ON                  -> assert
 *   - list_head / codec_list  -> dropped (single codec per run)
 *   - gffunc non-canonical    -> dropped (we only need gfpoly path)
 *   - rs_modnn(rs, x)         -> inline (x %% rs->nn)
 *   - EBADMSG                 -> RS_UNCORRECTABLE
 *
 * The port lands in the next commit, after the test vector fixture
 * has been validated against the mkfs.beamfs encoder.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <stdlib.h>

#include "rs_decode.h"

/*
 * Opaque handle. The real port will hold:
 *   - alpha_to[256]: GF(2^8) anti-log table
 *   - index_of[256]: GF(2^8) log table
 *   - genpoly[RS_NROOTS + 1]: generator polynomial, index form
 *   - iprim: prim-th root of 1, used during decoding
 *   - buffers for lambda/syn/b/t/omega/root/reg/loc (each nroots+1)
 * Sizes are constant for our single configuration (RS(255,239)),
 * so a static allocation is acceptable; we keep the opaque-handle
 * API to mirror the kernel's struct rs_control discipline.
 */
struct rs_codec {
	int placeholder;  /* sub-2.partial-2 stub; real fields in next commit */
};

struct rs_codec *rs_init(void)
{
	struct rs_codec *rs = calloc(1, sizeof(*rs));

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
