#!/usr/bin/env python3
"""Exp 3 packing-decouple plots.

Tests whether T=64 N=4 zipf bottleneck is per-bucket lock contention
(thpt flat across thread count) or thread-side packing cost (thpt scales
linearly with thread count).

Layout: shards=16 (fixed by N=4). threads_per_type ∈ {1, 2, 4, 8, 16, default(=8)}.
1 = max packing; 16 = no packing (1 thread per shard).

Output:
  exp3_thpt_vs_threads.png — 2 lines (zipf, uniform), median + per-rep dots
  exp3_summary_table.png   — table of medians
"""
import csv, os, statistics, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) >= 2:
    EXP3 = sys.argv[1]
else:
    # find the most-recent exp3 dir
    docs = "/home/yanwang/FUSEE/docs"
    cands = sorted([d for d in os.listdir(docs) if d.startswith("iter17A_exp3_packing_")])
    EXP3 = os.path.join(docs, cands[-1])
CSV = os.path.join(EXP3, "grid.csv")
print(f"EXP3 = {EXP3}")

# Load
data = defaultdict(lambda: defaultdict(list))  # data[dist][threads_force_str] = [thpt_Mops, ...]
with open(CSV) as f:
    for r in csv.DictReader(f):
        dist = r["dist"]; force = r["threads_force"]
        thpt = float(r["thpt"]) / 1e6
        if thpt > 0:
            data[dist][force].append(thpt)

force_ordering = ["1", "2", "4", "8", "16", "default"]
force_labels = {"1": "1 (max pack)", "2": "2", "4": "4", "8": "8 (default=8)", "16": "16 (1:1, no pack)", "default": "default(8)"}

# ----- Plot 1: thpt vs threads_force (annotated for oversubscription artifact) -----
fig, ax = plt.subplots(figsize=(12, 6.5))
colors = {"zipf": "#d62728", "uniform": "#1f77b4"}
markers = {"zipf": "o", "uniform": "s"}
x = np.arange(len(force_ordering))

# Shade the CPU-oversubscription artifact zone (force=8,16)
# pool_size=22, total threads = 3*force. force=8 → 24>22 (overlap); force=16 → 48>22 (severe)
ax.axvspan(2.5, 4.5, color="#ffe5e5", alpha=0.6, zorder=0)
ax.text(3.5, ax.get_ylim()[1]*0.95 if ax.get_ylim()[1] > 0 else 5, "CPU oversubscription\n(modulo-wrap artifact)",
        ha="center", va="top", fontsize=10, color="#aa0000", fontweight="bold",
        bbox=dict(boxstyle="round,pad=0.3", facecolor="white", edgecolor="#aa0000", linewidth=1))

# Plot ideal linear extrapolation from force=1
for dist in ("zipf", "uniform"):
    base = statistics.median(data[dist].get("1", []) or [0])
    ax.plot(x[:4], [base * v for v in [1, 2, 4, 8]],
            color=colors[dist], linestyle=":", alpha=0.5, linewidth=1.5,
            label=f"{dist} ideal-linear ext. (from force=1)" if dist == "zipf" else None)

for dist in ("zipf", "uniform"):
    medians = []
    for force in force_ordering:
        vs = data[dist].get(force, [])
        med = statistics.median(vs) if vs else float("nan")
        medians.append(med)
        for v in vs:
            ax.scatter([force_ordering.index(force) + (0.06 if dist=="uniform" else -0.06)], [v],
                       color=colors[dist], marker=markers[dist], alpha=0.35, s=40, edgecolor="none")
    ax.plot(x, medians, color=colors[dist], marker=markers[dist],
            linewidth=2, markersize=10, label=f"{dist} (median)", zorder=5)

# Annotate force=default specially
for dist in ("zipf", "uniform"):
    med = statistics.median(data[dist].get("default", []) or [0])
    ax.annotate(f"default = 8/7/7 = 22\n(fits CPU pool exactly)",
                xy=(5, med), xytext=(5.05, med - 1.0 if dist == "zipf" else med - 1.6),
                fontsize=8.5, color=colors[dist],
                arrowprops=dict(arrowstyle="->", color=colors[dist], alpha=0.6))

ax.set_xticks(x)
ax.set_xticklabels([force_labels[f] for f in force_ordering], rotation=10)
ax.set_xlabel("FUSEE_FORCE_THREADS_PER_TYPE  (3 × value = total receiver threads)", fontsize=11)
ax.set_ylabel("Cluster throughput (Mops/s)", fontsize=11)
ax.set_title("iter-17A Exp 3: packing decouple — T=64 N=4 worker_id, shards=16 fixed (3-rep median)\n"
             "Clean zone (force ≤ 4 & default): linear in receiver count → packing-bound, NOT lock-bound",
             fontsize=12, pad=10)
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=9)
ax.set_ylim(0, 7.5)

out1 = os.path.join(EXP3, "exp3_thpt_vs_threads.png")
plt.tight_layout()
plt.savefig(out1, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out1}")

# ----- Plot 2: summary table -----
fig, ax = plt.subplots(figsize=(11, 3.5))
ax.axis("off")
headers = ["Distribution"] + [force_labels[f] for f in force_ordering]
rows = []
for dist in ("zipf", "uniform"):
    row = [dist]
    for force in force_ordering:
        vs = data[dist].get(force, [])
        med = statistics.median(vs) if vs else None
        row.append(f"{med:.3f}" if med is not None else "—")
    rows.append(row)
tbl = ax.table(cellText=rows, colLabels=headers, loc="center", cellLoc="center", colLoc="center")
tbl.auto_set_font_size(False)
tbl.set_fontsize(10)
tbl.scale(1.0, 1.8)
for j in range(len(headers)):
    tbl[(0, j)].set_facecolor("#cfcfcf")
    tbl[(0, j)].set_text_props(weight="bold")
for i in range(1, 3):
    tbl[(i, 0)].set_text_props(weight="bold")
    tbl[(i, 0)].set_facecolor("#f0f0f0")
ax.set_title("iter-17A Exp 3: medians (3-rep), cluster Mops/s — T=64 N=4 worker_id xhost_write V=1024",
             fontsize=11.5, pad=10, weight="bold")
out2 = os.path.join(EXP3, "exp3_summary_table.png")
plt.tight_layout()
plt.savefig(out2, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out2}")

# Also stdout summary
print()
print(f"{'force':<14s} {'zipf':>10s} {'uniform':>10s}")
for force in force_ordering:
    z = statistics.median(data["zipf"].get(force, [0]) or [0])
    u = statistics.median(data["uniform"].get(force, [0]) or [0])
    print(f"{force_labels[force]:<14s} {z:>10.3f} {u:>10.3f}")
