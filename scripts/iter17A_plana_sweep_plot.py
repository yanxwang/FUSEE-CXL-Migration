#!/usr/bin/env python3
"""iter-17A Plan A sweep visualization.

Plots two figures:
1. Throughput vs T for N=0/4/8 + iter-15A baseline (line plot, log-Y)
2. Speedup (N=*/baseline) heatmap

Hardcoded data from single-rep sweep (post-ReservHandler-fix).
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_plana_singlerep_summary"
os.makedirs(OUT_DIR, exist_ok=True)

# T values
Ts = [1, 2, 4, 8, 16, 32, 64]
# Per-host Mops/s (single rep, ReservHandler-fix applied)
baseline_15A = [0.198, 0.299, 0.527, 0.554, 0.563, 0.612, 0.604]
n0 =           [0.236, 0.468, 0.829, 1.016, 1.126, 1.256, 1.225]
# N=4 and N=8 are same as N=0 for T<=4 (shards=1)
n4 =           [0.236, 0.468, 0.829, 0.681, 3.409, 4.414, 8.052]
n8 =           [0.236, 0.468, 0.829, 1.125, 2.268, 4.285, 7.491]

# ----- Figure 1: throughput lines -----
fig, ax = plt.subplots(1, 1, figsize=(10, 7))
ax.plot(Ts, baseline_15A, "o-", color="#888888", linewidth=2, markersize=9,
        label="iter-15A baseline (single receiver, no opt)")
ax.plot(Ts, n0,           "s-", color="#1f77b4", linewidth=2.5, markersize=9,
        label="N=0 (single shard, iter-17A path opt + ReservHandler fix)")
ax.plot(Ts, n4,           "^-", color="#d62728", linewidth=2.5, markersize=10,
        label="N=4 (multi-receiver, 4 workers/shard) ⭐")
ax.plot(Ts, n8,           "v-", color="#2ca02c", linewidth=2.5, markersize=9,
        label="N=8 (multi-receiver, 8 workers/shard)")

# Annotate peak T=64 N=4
ax.annotate(f"{n4[-1]:.2f} Mops/s\n(+{(n4[-1]/baseline_15A[-1]-1)*100:.0f}% vs baseline)",
            xy=(64, n4[-1]), xytext=(35, 6.5),
            fontsize=11, color="#d62728", fontweight="bold",
            arrowprops=dict(arrowstyle="->", color="#d62728", lw=1.5))

# Annotate regression cell T=8 N=4
ax.annotate(f"T=8 N=4 = {n4[3]:.2f}\n-{(1-n4[3]/n0[3])*100:.0f}% vs N=0 (regression)",
            xy=(8, n4[3]), xytext=(2.5, 0.30),
            fontsize=10, color="#d62728",
            arrowprops=dict(arrowstyle="->", color="#d62728", lw=1.2))

# Annotate iter-15A → N=0 baseline lift
ax.annotate(f"iter-17A N=0 baseline\n+83-105% vs iter-15A\n(ReservHandler pin + path opt)",
            xy=(32, n0[5]), xytext=(8, 2.2),
            fontsize=10, color="#1f77b4",
            arrowprops=dict(arrowstyle="->", color="#1f77b4", lw=1.2))

ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xticks(Ts)
ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T (workers per host)", fontsize=12)
ax.set_ylabel("Per-host throughput (Mops/s, log scale)", fontsize=12)
ax.set_title("iter-17A Plan A multi-ring/multi-receiver scaling — xhost_write zipf-0.99 V=1024",
             fontsize=13, pad=12)
ax.grid(True, alpha=0.3, which="both")
ax.legend(loc="upper left", fontsize=10)

# Right side: target line at 20 Mops/host = 10 Mops/host single-host
ax.axhline(y=10, color="#aa00aa", linestyle="--", alpha=0.7, linewidth=2)
ax.text(1.1, 10*1.1, "20 Mops/s cluster target (= 10 Mops/host)",
        fontsize=10, color="#aa00aa", fontweight="bold")

plt.tight_layout()
out1 = os.path.join(OUT_DIR, "plana_thpt_vs_T.png")
plt.savefig(out1, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out1}")

# ----- Figure 2: bar comparison at each T -----
fig, ax = plt.subplots(1, 1, figsize=(13, 7))
x = np.arange(len(Ts))
w = 0.2
ax.bar(x - 1.5*w, baseline_15A, w, label="iter-15A baseline", color="#888888")
ax.bar(x - 0.5*w, n0,           w, label="N=0 (iter-17A single-shard)", color="#1f77b4")
ax.bar(x + 0.5*w, n4,           w, label="N=4 (multi-recv)", color="#d62728")
ax.bar(x + 1.5*w, n8,           w, label="N=8 (multi-recv)", color="#2ca02c")
ax.set_xticks(x)
ax.set_xticklabels([f"T={t}" for t in Ts])
ax.set_yscale("log")
ax.set_ylabel("Per-host throughput (Mops/s, log)", fontsize=12)
ax.set_title("Plan A throughput per T (vs iter-15A baseline) — xhost_write zipf-0.99",
             fontsize=13, pad=10)
ax.grid(True, alpha=0.3, axis="y", which="both")
ax.legend(loc="upper left", fontsize=10)
# Annotate bars
for i, t in enumerate(Ts):
    bvals = [(baseline_15A[i], -1.5), (n0[i], -0.5), (n4[i], 0.5), (n8[i], 1.5)]
    for val, off in bvals:
        ax.text(x[i] + off*w, val*1.05, f"{val:.2f}",
                ha="center", va="bottom", fontsize=8, rotation=70)

plt.tight_layout()
out2 = os.path.join(OUT_DIR, "plana_thpt_bars.png")
plt.savefig(out2, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out2}")

# ----- Figure 3: speedup heatmap (vs iter-15A baseline) -----
speedup = np.array([
    [n0[i]/baseline_15A[i] for i in range(len(Ts))],
    [n4[i]/baseline_15A[i] for i in range(len(Ts))],
    [n8[i]/baseline_15A[i] for i in range(len(Ts))],
])
fig, ax = plt.subplots(1, 1, figsize=(12, 4))
im = ax.imshow(speedup, aspect="auto", cmap="RdYlGn", vmin=0.5, vmax=14)
ax.set_xticks(range(len(Ts)))
ax.set_xticklabels([str(t) for t in Ts])
ax.set_yticks([0, 1, 2])
ax.set_yticklabels(["N=0\n(single-shard)", "N=4\n(multi-recv)", "N=8\n(multi-recv)"])
ax.set_xlabel("T (workers per host)", fontsize=12)
ax.set_title("Speedup over iter-15A baseline (× multiplier) — Plan A",
             fontsize=13, pad=10)
for i in range(speedup.shape[0]):
    for j in range(speedup.shape[1]):
        val = speedup[i, j]
        txt_color = "black" if 0.8 < val < 8 else "white"
        ax.text(j, i, f"{val:.1f}×", ha="center", va="center",
                color=txt_color, fontsize=11, fontweight="bold")
plt.colorbar(im, ax=ax, label="speedup factor")
plt.tight_layout()
out3 = os.path.join(OUT_DIR, "plana_speedup_heatmap.png")
plt.savefig(out3, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out3}")

print("---")
print(f"OUT: {OUT_DIR}")
