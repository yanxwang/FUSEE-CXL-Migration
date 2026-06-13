#!/usr/bin/env python3
"""iter-17A Plan A 3-rep verify — show medians + reps as scatter."""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_plana_3rep_20260522_074211"
os.makedirs(OUT_DIR, exist_ok=True)

# 3-rep data: (T, N, median_Mops, [rep1, rep2, rep3])
cells = [
    (8,  0, 0.924, [0.924, 0.925, 0.923]),
    (8,  4, 1.281, [0.787, 1.281, 1.347]),
    (16, 4, 2.456, [2.456, 2.342, 2.813]),
    (32, 4, 2.540, [2.540, 2.489, 2.552]),
    (64, 4, 6.260, [6.260, 6.211, 6.665]),
    (64, 8, 6.172, [6.152, 6.251, 6.172]),
]
iter15A_base = {8: 0.554, 16: 0.563, 32: 0.612, 64: 0.604}

fig, ax = plt.subplots(1, 1, figsize=(11, 7))
labels = []
medians = []
reps_all = []
baselines = []
for T, N, med, reps in cells:
    label = f"T={T}\nN={N}"
    labels.append(label)
    medians.append(med)
    reps_all.append(reps)
    baselines.append(iter15A_base[T])

x = np.arange(len(labels))
w = 0.35

# baseline bars (gray)
ax.bar(x - w/2, baselines, w, color="#888888", label="iter-15A baseline (single recv)")
# median bars (red)
ax.bar(x + w/2, medians, w, color="#d62728", label="iter-17A Plan A (3-rep median)")

# scatter the individual reps for transparency
for i, reps in enumerate(reps_all):
    ax.scatter([x[i] + w/2] * len(reps), reps, color="black", s=22, zorder=3,
               label="individual reps" if i == 0 else None)

# annotate medians + speedup
for i, (T, N, med, reps) in enumerate(cells):
    sp = med / iter15A_base[T]
    ax.text(x[i] + w/2, med + 0.4, f"{med:.2f}\n({sp:.1f}×)",
            ha="center", va="bottom", fontsize=10, color="#d62728", fontweight="bold")
    ax.text(x[i] - w/2, baselines[i] + 0.1, f"{baselines[i]:.2f}",
            ha="center", va="bottom", fontsize=9, color="#444")

# 20 Mops/s cluster target line at 10 Mops/host
ax.axhline(y=10, color="#aa00aa", linestyle="--", alpha=0.7, linewidth=2)
ax.text(5.3, 10*1.05, "20 Mops/s cluster target (= 10 Mops/host)",
        fontsize=10, color="#aa00aa", fontweight="bold", ha="right")

ax.set_xticks(x)
ax.set_xticklabels(labels)
ax.set_ylabel("Per-host throughput (Mops/s)", fontsize=12)
ax.set_title("iter-17A Plan A 3-rep verified — xhost_write zipf-0.99 V=1024 (5M ops/host)",
             fontsize=13, pad=12)
ax.grid(True, alpha=0.3, axis="y")
ax.legend(loc="upper left", fontsize=10)
ax.set_ylim(0, 12)

plt.tight_layout()
out = os.path.join(OUT_DIR, "plana_3rep_verify.png")
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")
