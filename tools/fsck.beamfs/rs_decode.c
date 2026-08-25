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

/*
 * rs_decode_internal -- byte-exact port of lib/reed_solomon/decode_rs.c
 *
 * Operates on uint16_t buffers (kernel native width for the symsize=8
 * configuration). The wrapper rs_decode_subblock() does the 8<->16
 * conversion.
 *
 * Specialisation vs. the kernel decode_rs8:
 *   s = NULL          (no caller-provided syndromes)
 *   no_eras = 0       (no erasure positions)
 *   eras_pos = NULL
 *   invmsk = 0
 *   corr = NULL       (corrections applied in place to data/par)
 *
 * Returns the number of symbols corrected on success (0 if clean),
 * or RS_UNCORRECTABLE on errors beyond the correction radius.
 */
static int rs_decode_internal(struct rs_codec *rs,
			      uint16_t *data, uint16_t *par, int len,
			      int *positions)
{
	int        deg_lambda, el, deg_omega;
	int        i, j, r, k, pad;
	int        nn = rs->nn;
	int        nroots = rs->nroots;
	int        fcr = rs->fcr;
	int        prim = rs->prim;
	int        iprim = rs->iprim;
	uint16_t  *alpha_to = rs->alpha_to;
	uint16_t  *index_of = rs->index_of;
	uint16_t   q, tmp, num1, num2, den, discr_r, syn_error;
	int        count = 0;
	int        num_corrected;
	uint16_t  *s;
	uint16_t  *lambda = rs->buffers + RS_DECODE_LAMBDA * (nroots + 1);
	uint16_t  *syn    = rs->buffers + RS_DECODE_SYN    * (nroots + 1);
	uint16_t  *b      = rs->buffers + RS_DECODE_B      * (nroots + 1);
	uint16_t  *t      = rs->buffers + RS_DECODE_T      * (nroots + 1);
	uint16_t  *omega  = rs->buffers + RS_DECODE_OMEGA  * (nroots + 1);
	uint16_t  *root   = rs->buffers + RS_DECODE_ROOT   * (nroots + 1);
	uint16_t  *reg    = rs->buffers + RS_DECODE_REG    * (nroots + 1);
	uint16_t  *loc    = rs->buffers + RS_DECODE_LOC    * (nroots + 1);

	pad = nn - nroots - len;
	if (pad < 0 || pad >= nn - nroots)
		return RS_UNCORRECTABLE;

	/*
	 * Form the syndromes: evaluate data(x) at roots of g(x).
	 * invmsk=0 specialisation: the XOR term drops out.
	 */
	for (i = 0; i < nroots; i++)
		syn[i] = data[0];

	for (j = 1; j < len; j++) {
		for (i = 0; i < nroots; i++) {
			if (syn[i] == 0) {
				syn[i] = data[j];
			} else {
				syn[i] = data[j] ^
					alpha_to[rs_modnn(rs,
						index_of[syn[i]] + (fcr + i) * prim)];
			}
		}
	}

	for (j = 0; j < nroots; j++) {
		for (i = 0; i < nroots; i++) {
			if (syn[i] == 0) {
				syn[i] = par[j];
			} else {
				syn[i] = par[j] ^
					alpha_to[rs_modnn(rs,
						index_of[syn[i]] + (fcr + i) * prim)];
			}
		}
	}

	s = syn;

	/* Convert syndromes to index form, checking for nonzero condition. */
	syn_error = 0;
	for (i = 0; i < nroots; i++) {
		syn_error |= s[i];
		s[i] = index_of[s[i]];
	}
	if (!syn_error) {
		/* Codeword is clean. */
		return 0;
	}

	/* Berlekamp-Massey: determine error locator polynomial. */
	memset(&lambda[1], 0, nroots * sizeof(lambda[0]));
	lambda[0] = 1;

	for (i = 0; i < nroots + 1; i++)
		b[i] = index_of[lambda[i]];

	r = 0;
	el = 0;
	while (++r <= nroots) {
		discr_r = 0;
		for (i = 0; i < r; i++) {
			if ((lambda[i] != 0) && (s[r - i - 1] != nn)) {
				discr_r ^= alpha_to[rs_modnn(rs,
					index_of[lambda[i]] + s[r - i - 1])];
			}
		}
		discr_r = index_of[discr_r];
		if (discr_r == nn) {
			memmove(&b[1], b, nroots * sizeof(b[0]));
			b[0] = (uint16_t)nn;
		} else {
			t[0] = lambda[0];
			for (i = 0; i < nroots; i++) {
				if (b[i] != nn) {
					t[i + 1] = lambda[i + 1] ^
						alpha_to[rs_modnn(rs, discr_r + b[i])];
				} else {
					t[i + 1] = lambda[i + 1];
				}
			}
			if (2 * el <= r - 1) {
				el = r - el;
				for (i = 0; i <= nroots; i++) {
					b[i] = (lambda[i] == 0) ? (uint16_t)nn :
						rs_modnn(rs, index_of[lambda[i]]
							- discr_r + nn);
				}
			} else {
				memmove(&b[1], b, nroots * sizeof(b[0]));
				b[0] = (uint16_t)nn;
			}
			memcpy(lambda, t, (nroots + 1) * sizeof(t[0]));
		}
	}

	/* Convert lambda to index form and compute deg(lambda(x)). */
	deg_lambda = 0;
	for (i = 0; i < nroots + 1; i++) {
		lambda[i] = index_of[lambda[i]];
		if (lambda[i] != nn)
			deg_lambda = i;
	}
	if (deg_lambda == 0) {
		/* Syndrome non-zero but no locator -> uncorrectable. */
		return RS_UNCORRECTABLE;
	}

	/* Chien search: find roots of the error locator polynomial. */
	memcpy(&reg[1], &lambda[1], nroots * sizeof(reg[0]));
	count = 0;
	for (i = 1, k = iprim - 1; i <= nn;
	     i++, k = rs_modnn(rs, k + iprim)) {
		q = 1;
		for (j = deg_lambda; j > 0; j--) {
			if (reg[j] != nn) {
				reg[j] = rs_modnn(rs, reg[j] + j);
				q ^= alpha_to[reg[j]];
			}
		}
		if (q != 0)
			continue;
		if (k < pad) {
			/* Impossible error location -> uncorrectable. */
			return RS_UNCORRECTABLE;
		}
		root[count] = (uint16_t)i;
		loc[count]  = (uint16_t)k;
		if (++count == deg_lambda)
			break;
	}
	if (deg_lambda != count) {
		/* deg(lambda) != #roots -> uncorrectable. */
		return RS_UNCORRECTABLE;
	}

	/* Compute err evaluator omega(x) = s(x)*lambda(x) mod x^nroots. */
	deg_omega = deg_lambda - 1;
	for (i = 0; i <= deg_omega; i++) {
		tmp = 0;
		for (j = i; j >= 0; j--) {
			if ((s[i - j] != nn) && (lambda[j] != nn)) {
				tmp ^= alpha_to[rs_modnn(rs,
					s[i - j] + lambda[j])];
			}
		}
		omega[i] = index_of[tmp];
	}

	/* Compute error values; reuse b[] for correction pattern. */
	num_corrected = 0;
	for (j = count - 1; j >= 0; j--) {
		num1 = 0;
		for (i = deg_omega; i >= 0; i--) {
			if (omega[i] != nn)
				num1 ^= alpha_to[rs_modnn(rs,
					omega[i] + i * root[j])];
		}
		if (num1 == 0) {
			b[j] = 0;
			continue;
		}
		num2 = alpha_to[rs_modnn(rs, root[j] * (fcr - 1) + nn)];
		den = 0;
		/* lambda[i+1] for i even is the formal derivative. */
		for (i = (deg_lambda < nroots - 1 ? deg_lambda : nroots - 1) & ~1;
		     i >= 0; i -= 2) {
			if (lambda[i + 1] != nn) {
				den ^= alpha_to[rs_modnn(rs,
					lambda[i + 1] + i * root[j])];
			}
		}
		b[j] = alpha_to[rs_modnn(rs,
			index_of[num1] + index_of[num2] +
			nn - index_of[den])];
		num_corrected++;
	}

	/* Verify the syndrome of the inferred error matches. */
	for (i = 0; i < nroots; i++) {
		tmp = 0;
		for (j = 0; j < count; j++) {
			if (b[j] == 0)
				continue;
			k = (fcr + i) * prim * (nn - loc[j] - 1);
			tmp ^= alpha_to[rs_modnn(rs, index_of[b[j]] + k)];
		}
		if (tmp != alpha_to[s[i]])
			return RS_UNCORRECTABLE;
	}

	/* Apply the correction pattern to data and parity in place. */
	for (i = 0; i < count; i++) {
		if (loc[i] < (nn - nroots))
			data[loc[i] - pad] ^= b[i];
		else
			par[loc[i] - pad - len] ^= b[i];
	}

	/* Optional: report data positions corrected to the caller. */
	if (positions) {
		int n = 0;

		for (i = 0; i < count; i++) {
			if (loc[i] < (nn - nroots) && b[i] != 0)
				positions[n++] = loc[i] - pad;
		}
	}

	return num_corrected;
}

/*
 * Port of the reference encoder (mkfs.beamfs.c::encode_rs_userspace,
 * itself the userspace twin of the kernel's beamfs_rs_encode), adapted
 * to read genpoly/alpha_to/index_of from an already-initialised
 * struct rs_codec instead of static file-local tables. Structurally
 * identical LFSR: an attempt to derive this from decode_rs.c's index
 * conventions instead produced wrong parity on every test length
 * (172/211/239), so this follows the known-correct encoder's shape
 * exactly rather than re-deriving the arithmetic.
 */
void rs_encode_subblock(struct rs_codec *rs,
			const uint8_t *data, size_t len,
			uint8_t *parity)
{
	uint16_t *alpha_to = rs->alpha_to;
	uint16_t *index_of = rs->index_of;
	uint16_t *genpoly  = rs->genpoly;
	uint16_t par[RS_NROOTS];
	size_t i;

	memset(par, 0, sizeof(par));

	for (i = 0; i < len; i++) {
		uint16_t feedback = index_of[data[i] ^ par[0]];
		unsigned int j;

		if (feedback != rs->nn) {
			for (j = 0; j < (unsigned int)RS_NROOTS - 1; j++)
				par[j] = par[j + 1] ^
					alpha_to[rs_modnn(rs, feedback + genpoly[RS_NROOTS - 1 - j])];
			par[RS_NROOTS - 1] = alpha_to[rs_modnn(rs, feedback + genpoly[0])];
		} else {
			for (j = 0; j < (unsigned int)RS_NROOTS - 1; j++)
				par[j] = par[j + 1];
			par[RS_NROOTS - 1] = 0;
		}
	}

	for (i = 0; i < RS_NROOTS; i++)
		parity[i] = (uint8_t)(par[i] & 0xff);
}

int rs_decode_subblock(struct rs_codec *rs,
		       uint8_t *data, size_t len,
		       uint8_t *parity,
		       int *positions)
{
	uint16_t data16[RS_MAX_DATA_LEN];
	uint16_t par16[RS_NROOTS];
	size_t   i;
	int      rc;

	if (!rs || !data || !parity)
		return RS_UNCORRECTABLE;
	if (len == 0 || len > RS_MAX_DATA_LEN)
		return RS_UNCORRECTABLE;

	for (i = 0; i < len; i++)
		data16[i] = (uint16_t)data[i];
	for (i = 0; i < RS_NROOTS; i++)
		par16[i] = (uint16_t)parity[i];

	rc = rs_decode_internal(rs, data16, par16, (int)len, positions);

	if (rc >= 0) {
		for (i = 0; i < len; i++)
			data[i] = (uint8_t)(data16[i] & 0xff);
		for (i = 0; i < RS_NROOTS; i++)
			parity[i] = (uint8_t)(par16[i] & 0xff);
	}
	return rc;
}
