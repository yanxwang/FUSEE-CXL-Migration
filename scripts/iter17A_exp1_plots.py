#!/usr/bin/env python3
"""Exp 1 plots — single-key flood (T=32 host-pair maxes lock contention on
peer-owned hot bucket).

Plot 1: Table summary (Plan x N x T grid, medians, with row-coloring for
        the "2-stage pipeline cap" finding).
Plot 2: Grouped-bar visualization in two side-by-side panels (Plan A,
        Plan B). x = T, hue = N. Makes the asymmetry visible: Plan A
        N>=2 lifts ~1.9x to ~1.5 Mops then caps; Plan B all N stays at
        single-receiver ~0.8 Mops.
"""
import csv, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

EXP1 = "/home/yanwang/FUSEE/docs/iter17A_exp1_single_key_flood_20260523_022924"
CSV = os.path.join(EXP1, "grid.csv")

# Collect medians
data = defaultdict(lambda: defaultdict(list))  # data[(routing, N)][T] = [thpt_Mops, ...]
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"]); N = int(r["N"]); routing = r["routing"]
        thpt = float(r["thpt"]) / 1e6
        if thpt > 0:
            data[(routing, N)][T].append(thpt)
med = {k: {T: statistics.median(v) for T, v in by_T.items() if v} for k, by_T in data.items()}

Ts = [8, 16, 32, 64]
Ns = [0, 4, 8]
routings = ["worker_id", "key_hash"]

# ===== Plot 1: table summary =====
fig, ax = plt.subplots(figsize=(11, 4.5))
ax.axis("off")

headers = ["Plan", "N (shards_factor)"] + [f"T={T}" for T in Ts] + ["Note"]
cell_text = []
row_colors = []

def fmt(v): return f"{v:.3f}" if v is not None else "—"

# Order: A N=0, A N=4, A N=8, B N=0, B N=4, B N=8
plan_label = {"worker_id": "A (worker_id)", "key_hash": "B (key_hash)"}
notes = {
    ("worker_id", 0): "single recv baseline",
    ("worker_id", 4): "8 shards / 8 WriteRecv — 1.9x cap",
    ("worker_id", 8): "16 shards / 16 WriteRecv — same cap",
    ("key_hash", 0): "single recv (sanity match)",
    ("key_hash", 4): "hot key routes to 1 shard — no lift",
    ("key_hash", 8): "hot key routes to 1 shard — no lift",
}
for routing in routings:
    for N in Ns:
        row = [plan_label[routing], f"N={N}"]
        for T in Ts:
            row.append(fmt(med.get((routing, N), {}).get(T)))
        row.append(notes[(routing, N)])
        cell_text.append(row)
        # Color logic: highlight plan A N>=2 (cap) green, plan B N>=2 (no lift) light red
        if routing == "worker_id" and N >= 4:
            row_colors.append("#d9f5d4")
        elif routing == "key_hash" and N >= 4:
            row_colors.append("#fde4e4")
        else:
            row_colors.append("#f0f0f0")

tbl = ax.table(cellText=cell_text, colLabels=headers, loc="center",
               cellLoc="center", colLoc="center")
tbl.auto_set_font_size(False)
tbl.set_fontsize(10)
tbl.scale(1.0, 1.6)
# Header row styling
for j in range(len(headers)):
    tbl[(0, j)].set_facecolor("#cfcfcf")
    tbl[(0, j)].set_text_props(weight="bold")
# Per-row background
for i in range(1, len(cell_text) + 1):
    for j in range(len(headers)):
        tbl[(i, j)].set_facecolor(row_colors[i-1])
        if j == 0 or j == 1 or j == len(headers) - 1:
            tbl[(i, j)].set_text_props(weight="bold" if j < 2 else "normal",
                                       ha="left" if j == len(headers) - 1 else "center")
# Column widths
col_widths = [0.13, 0.10] + [0.08] * len(Ts) + [0.39]
for i in range(len(cell_text) + 1):
    for j, w in enumerate(col_widths):
        tbl[(i, j)].set_width(w)

ax.set_title("iter-17A Exp 1: single-key flood, cluster Mops/s (3-rep median)\n"
             "Both hosts UPDATE peer-owned hot bucket — maximum cross-host lock contention",
             fontsize=12, pad=12, weight="bold")

out1 = os.path.join(EXP1, "exp1_table_summary.png")
plt.savefig(out1, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out1}")

# ===== Plot 2: grouped bar — two panels side-by-side (Plan A, Plan B) =====
fig, axes = plt.subplots(1, 2, figsize=(15, 6), sharey=True)

x = np.arange(len(Ts))
width = 0.25
N_colors = {0: "#888888", 4: "#1f77b4", 8: "#d62728"}
N_labels = {0: "N=0 (single shard, 1 WriteRecv)",
            4: "N=4 (8 shards, 8 WriteRecv)",
            8: "N=8 (16 shards, 16 WriteRecv)"}

panel_titles = {
    "worker_id": "Plan A: worker_id routing\n(routes ops by sender thread id → spreads across shards)",
    "key_hash":  "Plan B: key_hash routing\n(routes ops by FNV(key) → hot key locked to 1 shard)",
}

for ax, routing in zip(axes, routings):
    for i, N in enumerate(Ns):
        ys = [med.get((routing, N), {}).get(T, 0) for T in Ts]
        bars = ax.bar(x + (i - 1) * width, ys, width,
                      label=N_labels[N], color=N_colors[N], edgecolor="black", linewidth=0.5)
        for b, y in zip(bars, ys):
            if y > 0:
                ax.text(b.get_x() + b.get_width()/2, y + 0.03, f"{y:.2f}",
                        ha="center", va="bottom", fontsize=8.5)
    # Annotate baseline + cap
    ax.axhline(y=0.8, color="#888888", linestyle=":", alpha=0.6, linewidth=1.2)
    ax.text(len(Ts) - 0.5, 0.83, "single-recv baseline ~0.8 Mops",
            fontsize=8.5, color="#555555", ha="right")
    if routing == "worker_id":
        ax.axhline(y=1.5, color="#2ca02c", linestyle="--", alpha=0.7, linewidth=1.2)
        ax.text(len(Ts) - 0.5, 1.53, "2-stage pipeline cap ~1.5 Mops (1.9× over single recv)",
                fontsize=8.5, color="#1c7c1c", ha="right")

    ax.set_xticks(x)
    ax.set_xticklabels([f"T={T}" for T in Ts])
    ax.set_xlabel("T (workers per host)", fontsize=11)
    ax.set_title(panel_titles[routing], fontsize=11.5)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    ax.set_ylim(0, 1.85)

axes[0].set_ylabel("Cluster throughput (Mops/s)", fontsize=11)

fig.suptitle("iter-17A Exp 1: single-key flood — Plan A 1.9× pipeline lift, Plan B flat at single-recv\n"
             "(both hosts UPDATE peer's host-owned key → all ops collide on one shared bucket)",
             fontsize=12.5, y=1.00)
plt.tight_layout(rect=[0, 0, 1, 0.94])
out2 = os.path.join(EXP1, "exp1_grouped_bar.png")
plt.savefig(out2, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out2}")

# Print summary
print("\nMedians (Mops/s cluster):")
print(f"{'Plan':<22s} " + " ".join(f"T={T:>3}" for T in Ts))
for routing in routings:
    for N in Ns:
        row = f"{plan_label[routing]} N={N}     "[:22] + " "
        for T in Ts:
            v = med.get((routing, N), {}).get(T)
            row += f"{v:>5.3f}  " if v is not None else "  NA   "
        print(row)
