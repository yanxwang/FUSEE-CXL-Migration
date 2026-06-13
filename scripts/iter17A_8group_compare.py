#!/usr/bin/env python3
"""iter-17A 8-group comparison: assemble medians from multiple sweeps.

Groups (cluster Mops/s, median of 3 reps unless noted):
  1. iter-15A baseline (single ring + receiver) — from iter15A_microbench_phase2 CSV (3-rep median)
  2. iter-17A path opt N=0 (single shard) — finals N=0 worker_id (matches key_hash within noise)
  3. Plan A worker_id N=0 — finals
  4. Plan A worker_id N=4 — main sweep + fill
  5. Plan A worker_id N=8 — fill + finals re-run
  6. Plan B key_hash N=0 — finals
  7. Plan B key_hash N=4 — main sweep + fill
  8. Plan B key_hash N=8 — fill

T = 1, 2, 4, 8, 16, 32, 64
"""
import csv, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_8group_final"
os.makedirs(OUT_DIR, exist_ok=True)

# Group 1: iter-15A baseline from CSV — median of 3 reps for xhost_write
def iter15A_baseline():
    csv_path = "/home/yanwang/FUSEE/docs/iter15A_microbench_phase2_20260520_063314/grid.csv"
    by_T = defaultdict(list)
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            if r["scenario"] != "xhost_write": continue
            if r["keydist"] != "zipf-0.99": continue
            if r["V"] != "1024": continue
            T = int(r["T"])
            by_T[T].append(float(r["thpt_Mops"]))
    return {T: statistics.median(v) for T, v in by_T.items()}

# Groups 2-8 from finals + fill + main sweep CSVs
SWEEP_DIRS = [
    "/home/yanwang/FUSEE/docs/iter17A_scaling_finals_20260522_084104",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_fill_20260522_081401",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_20260522_075307",
]

def collect_3rep(sweep_dirs):
    data = defaultdict(lambda: defaultdict(list))  # data[(N, routing)][T] = [thpt, ...]
    for d in sweep_dirs:
        csv_path = os.path.join(d, "grid.csv")
        if not os.path.exists(csv_path):
            print(f"missing {csv_path}")
            continue
        with open(csv_path) as f:
            for r in csv.DictReader(f):
                try:
                    T = int(r["T"])
                    N = int(r["N"])
                    routing = r["routing"]
                    thpt_col = "thpt" if "thpt" in r else ("thpt_cluster_ops" if "thpt_cluster_ops" in r else "thpt_h0_ops")
                    thpt = float(r[thpt_col]) / 1e6  # to Mops
                    if thpt > 0:
                        data[(N, routing)][T].append(thpt)
                except (KeyError, ValueError):
                    continue
    medians = {}
    for k, by_T in data.items():
        medians[k] = {T: statistics.median(v) for T, v in by_T.items() if v}
    return medians

g1 = iter15A_baseline()
combined = collect_3rep(SWEEP_DIRS)

# Build 8-group dataset
# Group 2 = iter-17A path opt N=0 = Plan A N=0 (same code path)
g2 = combined.get((0, "worker_id"), {})
g3 = combined.get((0, "worker_id"), {})   # same as g2
g4 = combined.get((4, "worker_id"), {})
g5 = combined.get((8, "worker_id"), {})
g6 = combined.get((0, "key_hash"), {})
g7 = combined.get((4, "key_hash"), {})
g8 = combined.get((8, "key_hash"), {})

Ts = [1, 2, 4, 8, 16, 32, 64]
groups = [
    ("1. iter-15A baseline (single ring+recv)", g1, "#888888", "o", "-"),
    ("2. iter-17A path opt N=0",                g2, "#1f77b4", "s", "-"),
    ("3. Plan A worker_id N=0",                 g3, "#1f77b4", "x", "--"),
    ("4. Plan A worker_id N=4",                 g4, "#d62728", "^", "-"),
    ("5. Plan A worker_id N=8",                 g5, "#d62728", "v", "--"),
    ("6. Plan B key_hash  N=0",                 g6, "#2ca02c", "x", "--"),
    ("7. Plan B key_hash  N=4",                 g7, "#2ca02c", "^", "-"),
    ("8. Plan B key_hash  N=8",                 g8, "#9467bd", "v", "--"),
]

# Write CSV table
csv_path = os.path.join(OUT_DIR, "8group_median_table.csv")
with open(csv_path, "w") as f:
    f.write("Group," + ",".join(f"T={T}" for T in Ts) + "\n")
    for label, d, _, _, _ in groups:
        row = [label]
        for T in Ts:
            v = d.get(T, None)
            row.append(f"{v:.3f}" if v is not None else "NA")
        f.write(",".join(row) + "\n")
print(f"wrote {csv_path}")

# ----- Figure 1: side-by-side log-Y + linear-Y -----
fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(20, 8))

def draw_panel(ax, yscale_log):
    for label, d, color, marker, ls in groups:
        ys = [d.get(T, float("nan")) for T in Ts]
        ax.plot(Ts, ys, color=color, marker=marker, linestyle=ls,
                linewidth=2, markersize=9, label=label)
    # 20 Mops cluster target line
    ax.axhline(y=20, color="#aa00aa", linestyle="--", alpha=0.7, linewidth=2)
    target_y = 21 if yscale_log else 19
    ax.text(1.05, target_y, "20 Mops/s cluster target", fontsize=10,
            color="#aa00aa", fontweight="bold",
            va="bottom" if yscale_log else "top")
    ax.set_xscale("log", base=2)
    if yscale_log:
        ax.set_yscale("log")
        ax.set_ylabel("Cluster throughput (Mops/s, log scale)", fontsize=12)
        ax.set_title("log-Y", fontsize=12)
    else:
        ax.set_ylim(0, 22)
        ax.set_ylabel("Cluster throughput (Mops/s, linear scale)", fontsize=12)
        ax.set_title("linear-Y", fontsize=12)
    ax.set_xticks(Ts)
    ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("T (workers per host)", fontsize=12)
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="upper left", fontsize=9, ncol=2)

draw_panel(ax_log, yscale_log=True)
draw_panel(ax_lin, yscale_log=False)

fig.suptitle("iter-17A 8-group scaling comparison — xhost_write zipf-0.99 V=1024 (cluster Mops/s, 3-rep median)",
             fontsize=14, y=0.99)
plt.tight_layout(rect=[0, 0, 1, 0.97])
out1 = os.path.join(OUT_DIR, "8group_thpt_vs_T.png")
plt.savefig(out1, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out1}")

# ----- Figure 2: speedup vs iter-15A baseline (heatmap) -----
labels_short = ["iter-15A", "iter17A N=0", "A N=0", "A N=4", "A N=8", "B N=0", "B N=4", "B N=8"]
mat = np.zeros((8, len(Ts)))
for i, (_, d, _, _, _) in enumerate(groups):
    for j, T in enumerate(Ts):
        b = g1.get(T, None)
        v = d.get(T, None)
        if b is not None and b > 0 and v is not None:
            mat[i, j] = v / b
        else:
            mat[i, j] = float("nan")

fig, ax = plt.subplots(1, 1, figsize=(10, 6))
im = ax.imshow(mat, aspect="auto", cmap="RdYlGn", vmin=0.8, vmax=14)
ax.set_xticks(range(len(Ts)))
ax.set_xticklabels([f"T={T}" for T in Ts])
ax.set_yticks(range(8))
ax.set_yticklabels(labels_short)
ax.set_title("Speedup vs iter-15A baseline (× multiplier)", fontsize=13, pad=10)
for i in range(8):
    for j in range(len(Ts)):
        v = mat[i, j]
        if not np.isnan(v):
            color = "black" if 1 < v < 8 else "white"
            ax.text(j, i, f"{v:.1f}×", ha="center", va="center",
                    color=color, fontsize=10, fontweight="bold")
plt.colorbar(im, ax=ax, label="× speedup")
plt.tight_layout()
out2 = os.path.join(OUT_DIR, "8group_speedup_heatmap.png")
plt.savefig(out2, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out2}")

# ----- Print summary table to stdout -----
print()
print(f"{'Group':<40s} " + " ".join(f"T={T:>3}" for T in Ts))
for label, d, _, _, _ in groups:
    line = f"{label:<40s} "
    for T in Ts:
        v = d.get(T, None)
        line += f"{v:>5.3f}  " if v is not None else "  NA   "
    print(line)
