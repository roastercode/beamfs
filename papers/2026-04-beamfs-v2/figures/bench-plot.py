#!/usr/bin/env python3.13
"""Generate paper-grade figures from Palier 4 fio raw JSON outputs.

Outputs:
  bench-b1-read.png   read cold latency, BEAMFS scheme=5 vs ext4
  bench-b2-write.png  write+fsync latency, BEAMFS scheme=5 vs ext4
  bench-b3-inline.png INLINE canary read latency (single fs)
"""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent / "empirical" / "raw"
OUT = Path(__file__).resolve().parent

def load_clat_pct(path, mode):
    """Return (p50, p95, p99) in microseconds from a fio JSON file.

    mode = 'read' or 'write'. Reads jobs[0][mode].clat_ns.percentile.
    """
    d = json.loads(path.read_text())
    pct = d["jobs"][0][mode]["clat_ns"]["percentile"]
    return (pct["50.000000"] / 1000.0,
            pct["95.000000"] / 1000.0,
            pct["99.000000"] / 1000.0)

def collect(prefix, fs, mode):
    out = {"p50": [], "p95": [], "p99": []}
    for run in range(1, 11):
        path = ROOT / f"{prefix}-{fs}-r{run}.json"
        p50, p95, p99 = load_clat_pct(path, mode)
        out["p50"].append(p50)
        out["p95"].append(p95)
        out["p99"].append(p99)
    return out

def boxplot_pair(ax, title, beamfs, ext4, ylabel="latency (us)"):
    data = [beamfs["p50"], ext4["p50"],
            beamfs["p95"], ext4["p95"],
            beamfs["p99"], ext4["p99"]]
    labels = ["BEAMFS\np50", "ext4\np50",
              "BEAMFS\np95", "ext4\np95",
              "BEAMFS\np99", "ext4\np99"]
    bp = ax.boxplot(data, tick_labels=labels, patch_artist=True, widths=0.6)
    colors = ["#1f77b4", "#ff7f0e"] * 3
    for patch, c in zip(bp["boxes"], colors):
        patch.set_facecolor(c)
        patch.set_alpha(0.6)
    ax.set_yscale("log")
    ax.set_ylabel(ylabel + " (log scale)")
    ax.set_title(title)
    ax.grid(True, which="both", axis="y", alpha=0.3)

# B1 read cold
b1_beamfs = collect("b1", "inode", "read")
b1_ext4   = collect("b1", "ext4",  "read")
fig, ax = plt.subplots(figsize=(9, 5))
boxplot_pair(ax,
    "B1 -- Read cold latency (4K psync, 2 MB file, 10 runs)\n"
    "BEAMFS scheme=5 INODE_UNIVERSAL vs ext4 baseline",
    b1_beamfs, b1_ext4)
fig.tight_layout()
fig.savefig(OUT / "bench-b1-read.png", dpi=150)
print("wrote bench-b1-read.png")

# B2 write+fsync
b2_beamfs = collect("b2", "inode", "write")
b2_ext4   = collect("b2", "ext4",  "write")
fig, ax = plt.subplots(figsize=(9, 5))
boxplot_pair(ax,
    "B2 -- Write+fsync latency (4K psync, 2 MB file, 10 runs)\n"
    "BEAMFS scheme=5 INODE_UNIVERSAL vs ext4 baseline",
    b2_beamfs, b2_ext4)
fig.tight_layout()
fig.savefig(OUT / "bench-b2-write.png", dpi=150)
print("wrote bench-b2-write.png")

# B3 INLINE canary (single fs, no comparison)
b3 = {"p50": [], "p95": [], "p99": []}
for run in range(1, 11):
    path = ROOT / f"b3-inline-r{run}.json"
    p50, p95, p99 = load_clat_pct(path, "read")
    b3["p50"].append(p50)
    b3["p95"].append(p95)
    b3["p99"].append(p99)

fig, ax = plt.subplots(figsize=(7, 5))
data = [b3["p50"], b3["p95"], b3["p99"]]
labels = ["p50", "p95", "p99"]
bp = ax.boxplot(data, tick_labels=labels, patch_artist=True, widths=0.5)
for patch in bp["boxes"]:
    patch.set_facecolor("#2ca02c")
    patch.set_alpha(0.6)
ax.set_yscale("log")
ax.set_ylabel("latency (us) (log scale)")
ax.set_title(
    "B3 -- INLINE read sanity on canary fixture (3824 B)\n"
    "scheme=2 UNIVERSAL_INLINE, hot path, 10 runs ~38000 reads each")
ax.grid(True, which="both", axis="y", alpha=0.3)
fig.tight_layout()
fig.savefig(OUT / "bench-b3-inline.png", dpi=150)
print("wrote bench-b3-inline.png")
