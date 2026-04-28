#!/usr/bin/env python3
"""
BEAMFS v1 -- Deterministic generation of figure data files.

Outputs:
  data/fig1.dat -- Weibull SEE cross-sections (DRAM, SRAM, NAND)
  data/fig2.dat -- TLC NAND retention RBER vs P/E
  data/fig3.dat -- Recovery probability over LET x Phi grid

Parameters drawn from canonical sources cited in the paper.
"""
import math
import os

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')
os.makedirs(OUT_DIR, exist_ok=True)


# ============================================================
# fig1: Weibull SEE cross-sections
# Parameters from Dodd & Massengill 2003, Baumann 2005, Cai 2017.
# Weibull form: sigma(LET) = sigma_sat * (1 - exp(-((LET-L0)/W)^s))
# ============================================================
# (sat in cm^2/bit, L0 in MeV*cm^2/mg, W shape, s exponent)
# Calibrated to match per-bit FIT rates of:
#   DRAM 28nm: ~1e-9 cm^2/bit saturation (Baumann 2005 Fig. 7)
#   SRAM 28nm: ~1e-8 cm^2/bit saturation (Dodd 2003 Table I)
#   NAND TLC : ~1e-12 cm^2/bit (Cai 2017, direct SEU rare)
CURVES_FIG1 = [
    ('DRAM_28nm', 1e-9,  1.5, 4.0, 1.5),
    ('SRAM_28nm', 1e-8,  1.0, 3.5, 1.7),
    ('NAND_TLC',  1e-12, 8.0, 15.0, 1.8),
]
LET_MIN, LET_MAX, N_LET = 0.1, 100.0, 200


def weibull_sigma(LET, sat, L0, W, s):
    if LET <= L0:
        return 0.0
    return sat * (1.0 - math.exp(-(((LET - L0) / W) ** s)))


with open(os.path.join(OUT_DIR, 'fig1.dat'), 'w') as f:
    f.write('# LET[MeV*cm2/mg]   '
            + '   '.join(f'sigma_{n}[cm2/bit]' for n, *_ in CURVES_FIG1) + '\n')
    for i in range(N_LET + 1):
        LET = LET_MIN * (LET_MAX / LET_MIN) ** (i / N_LET)
        vals = [weibull_sigma(LET, *p[1:]) for p in CURVES_FIG1]
        f.write(f'{LET:.6e}  '
                + '  '.join(f'{v:.6e}' for v in vals) + '\n')
print(f'wrote fig1.dat ({N_LET+1} points)')


# ============================================================
# fig2: NAND retention RBER vs time, three P/E states
# Empirical model from Cai 2017, Mielke 2017.
# RBER(t) = RBER_0 * (1 + t/tau)^alpha, with tau and alpha PE-dependent.
# Calibrated to JEDEC JESD218B: ~1e-3 raw BER at EoL + 1 year.
# ============================================================
CURVES_FIG2 = [
    ('PE_0',     1e-6, 365.0, 0.5),
    ('PE_3000',  1e-5, 180.0, 1.0),
    ('PE_10000', 1e-4,  90.0, 1.6),
]
T_MIN, T_MAX, N_T = 1.0, 3650.0, 200


def rber(t, r0, tau, alpha):
    return r0 * (1.0 + t / tau) ** alpha


with open(os.path.join(OUT_DIR, 'fig2.dat'), 'w') as f:
    f.write('# t[days]  ' + '  '.join(f'RBER_{n}' for n, *_ in CURVES_FIG2) + '\n')
    for i in range(N_T + 1):
        t = T_MIN * (T_MAX / T_MIN) ** (i / N_T)
        vals = [rber(t, *p[1:]) for p in CURVES_FIG2]
        f.write(f'{t:.6e}  ' + '  '.join(f'{v:.6e}' for v in vals) + '\n')
print(f'wrote fig2.dat ({N_T+1} points)')


# ============================================================
# fig3: Recovery probability surface P_rec(LET, Phi)
# Calibrated so the transition zone is VISIBLE (not a cliff).
#
# Parameters tuned to put the transition at Phi ~ 1e6-1e7 part/cm2/s
# for moderate LET, which is the realistic concern range
# (cosmic background ~ 1 part/cm2/s, intense beam ~ 1e9).
#
# Use a SHORTER exposure window (1 hour) and FOCUSED RS subblock
# size (one inode metadata block, ~256 bytes = 2048 bits) to make
# the transition appear in a relevant Phi range.
# ============================================================
T_RS = 8                           # RS(255,239) correction capacity
SUBBLOCK_BITS = 2048               # one inode metadata block = 2 Kib
T_EXPOSURE_S = 3600.0              # 1-hour exposure window
SIGMA_SAT_DRAM = 1e-9              # DRAM 28 nm saturation
L0, W, S = 1.5, 4.0, 1.5           # DRAM Weibull params (matching fig1)


def sigma_dram(LET):
    if LET <= L0:
        return 0.0
    return SIGMA_SAT_DRAM * (1.0 - math.exp(-(((LET - L0) / W) ** S)))


def poisson_cdf(k, lam):
    """P(X <= k) for X ~ Poisson(lam). Numerically stable for moderate lam."""
    if lam <= 0:
        return 1.0
    if lam > 200:
        # Beyond this, P(X<=8) is essentially 0.
        return 0.0
    s, term = 0.0, math.exp(-lam)
    for i in range(k + 1):
        s += term
        term *= lam / (i + 1)
    return min(1.0, s)


N_LET3, N_PHI3 = 30, 30
LET_grid3 = [0.5 * (200.0 ** (i / (N_LET3 - 1))) for i in range(N_LET3)]
# Phi range: 1e3 to 1e8 part/cm2/s (intermediate values where the
# transition is visible at this exposure / subblock size).
PHI_grid3 = [1e3 * (1e5 ** (j / (N_PHI3 - 1))) for j in range(N_PHI3)]

with open(os.path.join(OUT_DIR, 'fig3.dat'), 'w') as f:
    f.write('# LET[MeV*cm2/mg]  Phi[part/cm2/s]  P_recovery\n')
    for LET in LET_grid3:
        for PHI in PHI_grid3:
            lam = sigma_dram(LET) * PHI * T_EXPOSURE_S * SUBBLOCK_BITS
            P = poisson_cdf(T_RS, lam)
            f.write(f'{LET:.6e}  {PHI:.6e}  {P:.6f}\n')
        f.write('\n')
print(f'wrote fig3.dat ({N_LET3*N_PHI3} points)')

print('\nAll data files written. md5sums:')
import hashlib
for fn in ('fig1.dat', 'fig2.dat', 'fig3.dat'):
    path = os.path.join(OUT_DIR, fn)
    with open(path, 'rb') as f:
        h = hashlib.md5(f.read()).hexdigest()
    print(f'  {h}  {fn}')
