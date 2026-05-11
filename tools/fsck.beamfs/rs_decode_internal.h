/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck.beamfs - Reed-Solomon decoder internals (userland)
 *
 * Exposes struct rs_codec layout to allow validation tests to
 * compare kernel-style tables against the mkfs encoder. Public
 * users of the decoder should include rs_decode.h only and treat
 * struct rs_codec as opaque.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef FSCK_BEAMFS_RS_DECODE_INTERNAL_H
#define FSCK_BEAMFS_RS_DECODE_INTERNAL_H

#include <stdint.h>

#include "rs_decode.h"

/* Decode scratch-buffer indices (kernel reed_solomon.c convention). */
enum {
	RS_DECODE_LAMBDA,
	RS_DECODE_SYN,
	RS_DECODE_B,
	RS_DECODE_T,
	RS_DECODE_OMEGA,
	RS_DECODE_ROOT,
	RS_DECODE_REG,
	RS_DECODE_LOC,
	RS_DECODE_NUM_BUFFERS
};

/*
 * RS codec state. Fixed-size for the single configuration we ship
 * (RS(255,239), symsize=8). The kernel uses heap-allocated arrays
 * because it generalises over symsize; we don't.
 *
 * Fields match struct rs_codec in linux/include/linux/rslib.h for
 * byte-exact agreement with the kernel decoder semantics. The list
 * head and refcount fields are dropped (single user per run).
 */
struct rs_codec {
	int      mm;                                       /* bits per symbol */
	int      nn;                                       /* (1<<mm)-1 */
	int      nroots;                                   /* parity symbols */
	int      fcr;                                      /* first consec root */
	int      prim;                                     /* primitive element */
	int      iprim;                                    /* prim-th root of 1 */
	int      gfpoly;                                   /* primitive polynomial */
	uint16_t alpha_to[256];                            /* log lookup */
	uint16_t index_of[256];                            /* antilog lookup */
	uint16_t genpoly[RS_NROOTS + 1];                   /* generator poly */
	uint16_t buffers[RS_DECODE_NUM_BUFFERS * (RS_NROOTS + 1)];
	/* scratch space for decode_rs (lambda/syn/b/t/omega/root/reg/loc) */
};

#endif /* FSCK_BEAMFS_RS_DECODE_INTERNAL_H */
