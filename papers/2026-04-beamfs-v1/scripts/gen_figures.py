#!/usr/bin/env python3
"""
BEAMFS v1 -- Render figures from data files.

Reads data/figX.dat (produced by gen_data.py) and produces
figures/figX.pdf via matplotlib.
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
DATA = os.path.join(ROOT, 'data')
OUT = os.path.join(ROOT, 'figures')
os.makedirs(OUT, exist_ok=True)

# Common typographic settings (matplotlib defaults sized to match
# IEEE caption font, ~9pt; figures use 7-8pt for labels).
RC = {
    'font.family': 'serif',
    'font.size': 8,
    'axes.labelsize': 8,
    'axes.titlesize': 9,
    'xtick.labelsize': 7,
    'ytick.labelsize': 7,
    'legend.fontsize': 7,
    'figure.dpi': 300,
}
plt.rcParams.update(RC)


# ============================================================
# Fig 1: SEE cross-section curves
# ============================================================
data1 = np.loadtxt(os.path.join(DATA, 'fig1.dat'))
LET = data1[:, 0]

fig, ax = plt.subplots(figsize=(9 / 2.54, 5.5 / 2.54))
ax.loglog(LET, data1[:, 1], lw=1.6, color='#1f4e79', label='DRAM 28 nm')
ax.loglog(LET, data1[:, 2], lw=1.6, color='#a04000', label='SRAM 28 nm')
ax.loglog(LET, data1[:, 3], lw=1.6, color='#4d6b3e', label='NAND TLC')
ax.set_xlabel(r'LET (MeV$\cdot$cm$^2$/mg)')
ax.set_ylabel(r'$\sigma$(LET) (cm$^2$/bit)')
ax.set_xlim(0.1, 100)
ax.set_ylim(1e-15, 1e-7)
ax.grid(True, which='major', color='#cccccc', lw=0.4)
ax.grid(False, which='minor')
ax.spines['top'].set_visible(False)
ax.spines['right'].set_visible(False)
ax.legend(loc='center left', bbox_to_anchor=(1.02, 0.5),
          frameon=True, framealpha=1.0, edgecolor='black',
          borderpad=0.5)
plt.tight_layout()
plt.savefig(os.path.join(OUT, 'fig1.pdf'),
            bbox_inches='tight', pad_inches=0.05)
plt.close()
print('wrote figures/fig1.pdf')


# ============================================================
# Fig 2: NAND retention RBER
# ============================================================
data2 = np.loadtxt(os.path.join(DATA, 'fig2.dat'))
t = data2[:, 0]

fig, ax = plt.subplots(figsize=(9 / 2.54, 5.5 / 2.54))
ax.loglog(t, data2[:, 1], lw=1.6, color='#1f4e79', label='Fresh (P/E=0)')
ax.loglog(t, data2[:, 2], lw=1.6, color='#a04000', label='Mid-life (P/E=3000)')
ax.loglog(t, data2[:, 3], lw=1.6, color='#7f1d1d', label='EoL (P/E=10000)')
ax.set_xlabel('Retention age (days)')
ax.set_ylabel('Raw bit-error rate (RBER)')
ax.set_xlim(1, 3650)
ax.set_ylim(1e-7, 1e-1)
ax.grid(True, which='major', color='#cccccc', lw=0.4)
ax.grid(False, which='minor')
ax.spines['top'].set_visible(False)
ax.spines['right'].set_visible(False)
ax.legend(loc='center left', bbox_to_anchor=(1.02, 0.5),
          frameon=True, framealpha=1.0, edgecolor='black',
          borderpad=0.5)
plt.tight_layout()
plt.savefig(os.path.join(OUT, 'fig2.pdf'),
            bbox_inches='tight', pad_inches=0.05)
plt.close()
print('wrote figures/fig2.pdf')


# ============================================================
# Fig 3: 3D recovery surface
# ============================================================
data3 = []
block = []
with open(os.path.join(DATA, 'fig3.dat')) as f:
    for line in f:
        line = line.strip()
        if line.startswith('#') or not line:
            if block:
                data3.append(block)
                block = []
            continue
        parts = line.split()
        if len(parts) == 3:
            block.append([float(p) for p in parts])
    if block:
        data3.append(block)

arr = np.array(data3)  # shape (n_LET, n_Phi, 3)
LET_g = arr[:, :, 0]
Phi_g = arr[:, :, 1]
P_g = arr[:, :, 2]

fig = plt.figure(figsize=(10 / 2.54, 7 / 2.54))
ax = fig.add_subplot(111, projection='3d')

sc = ax.scatter(np.log10(LET_g.flatten()),
                np.log10(Phi_g.flatten()),
                P_g.flatten(),
                c=P_g.flatten(), cmap='plasma',
                s=10, marker='o', alpha=0.85,
                edgecolors='none')

ax.set_xlabel(r'$\log_{10}$ LET', labelpad=2)
ax.set_ylabel(r'$\log_{10}\Phi$', labelpad=2)
ax.set_zlabel(r'$P_{\mathrm{rec}}$', labelpad=2)
ax.tick_params(axis='both', labelsize=6, pad=1)
ax.set_zlim(0, 1.05)

cbar = fig.colorbar(sc, ax=ax, fraction=0.04, pad=0.10, shrink=0.8)
cbar.set_label(r'$P_{\mathrm{rec}}$', fontsize=7)
cbar.ax.tick_params(labelsize=6)

ax.view_init(elev=22, azim=-65)
plt.tight_layout()
plt.savefig(os.path.join(OUT, 'fig3.pdf'),
            bbox_inches='tight', pad_inches=0.05)
plt.close()
print('wrote figures/fig3.pdf')

print('\nAll figures rendered.')
