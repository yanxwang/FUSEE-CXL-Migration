#!/usr/bin/env python3
"""Two-panel comparison: baseline A (A_old), A-v2 (A_opt), B, C.

Left panel: write latency per op (log scale, us).
Right panel: throughput per host (kops/s, linear).

Numbers are from docs/option_a_side_track.md and option_a_perf_analysis.md,
measured on emr devdax at 4 proc x 1 thread x 300 pure writes (wratio=1.0),
default bucket count, cxl_shm_profiling/bench/ycsb_abc_bench.c.
"""
import os
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    import sys; sys.exit("matplotlib required")

protocols = ["A_old", "A_opt", "B", "C"]
# Use midpoints of measured ranges for a clean single-bar chart.
latency_us   = [4000, 21, 13, 9]
thpt_k_host  = [2.5, 47, 78, 107]
# Range bars for the ones that had observed ranges (baseline A was singular).
latency_err_low  = [0,  3, 3, 2]
latency_err_high = [0,  4, 3, 2]
thpt_err_low     = [0,  7, 16, 26]
thpt_err_high    = [0,  8, 17, 26]

colors = {"A_old": "#b2182b", "A_opt": "#ef8a62",
          "B":     "#67a9cf", "C":     "#2166ac"}

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))

x = np.arange(len(protocols))

ax1.bar(x, latency_us, yerr=[latency_err_low, latency_err_high],
        color=[colors[p] for p in protocols], edgecolor="black",
        capsize=4)
ax1.set_yscale("log")
ax1.set_ylabel("write latency (μs, log scale)")
ax1.set_title("Writer avg latency (emr devdax, 4 proc × 300 writes)")
ax1.set_xticks(x); ax1.set_xticklabels(protocols)
ax1.grid(True, axis="y", which="both", alpha=0.3)
for i, v in enumerate(latency_us):
    if v >= 1000:
        lbl = f"{v/1000:.1f} ms"
    else:
        lbl = f"{v} μs"
    ax1.text(i, v * 1.25, lbl, ha="center", fontsize=10)

ax2.bar(x, thpt_k_host, yerr=[thpt_err_low, thpt_err_high],
        color=[colors[p] for p in protocols], edgecolor="black",
        capsize=4)
ax2.set_ylabel("throughput per host (k ops/s)")
ax2.set_title("Per-host write throughput (same setup)")
ax2.set_xticks(x); ax2.set_xticklabels(protocols)
ax2.grid(True, axis="y", alpha=0.3)
for i, v in enumerate(thpt_k_host):
    ax2.text(i, v + 4, f"{v:g}k", ha="center", fontsize=10)

fig.suptitle("Option A side-track: baseline vs optimized (A_opt = A-v2 SPSC ring)\n"
             "B and C are the sync-free reference protocols",
             fontsize=11, y=1.02)
fig.tight_layout()
out = os.path.join(os.path.dirname(__file__), "option_a_side_track_compare.png")
fig.savefig(out, dpi=150, bbox_inches="tight")
print(f"wrote {out}")
