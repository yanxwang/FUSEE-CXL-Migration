#!/usr/bin/env python3
"""Single-panel dual-axis comparison of A_old, A_opt, B, and C.

Left y-axis: write latency per op (log scale, us).
Right y-axis: throughput per host (kops/s, linear).

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
latency_us  = [4000, 21, 13, 9]
thpt_k_host = [2.5, 47, 78, 107]

fig, ax1 = plt.subplots(figsize=(9, 5))

x = np.arange(len(protocols))
bar_w = 0.35

lat_color  = "#b2182b"
thpt_color = "#2166ac"

b1 = ax1.bar(x - bar_w/2, latency_us, bar_w,
             color=lat_color, edgecolor="black", label="write latency")
ax1.set_yscale("log")
ax1.set_ylabel("write latency per op (μs, log scale)", color=lat_color)
ax1.tick_params(axis="y", labelcolor=lat_color)
ax1.set_xticks(x)
ax1.set_xticklabels(protocols)

ax2 = ax1.twinx()
b2 = ax2.bar(x + bar_w/2, thpt_k_host, bar_w,
             color=thpt_color, edgecolor="black", label="throughput / host")
ax2.set_ylabel("throughput per host (k ops/s)", color=thpt_color)
ax2.tick_params(axis="y", labelcolor=thpt_color)

# Value annotations.
for i, v in enumerate(latency_us):
    lbl = f"{v/1000:.1f} ms" if v >= 1000 else f"{v} μs"
    ax1.text(i - bar_w/2, v * 1.25, lbl, ha="center", fontsize=9,
             color=lat_color)
for i, v in enumerate(thpt_k_host):
    ax2.text(i + bar_w/2, v + 3, f"{v:g}k", ha="center", fontsize=9,
             color=thpt_color)

ax1.set_title("Option A_old, A_opt, B, and C protocol comparison")
ax1.grid(True, axis="y", which="both", alpha=0.3)

lines = [b1, b2]
labels = ["write latency", "throughput / host"]
ax1.legend(lines, labels, loc="upper right")

fig.tight_layout()
out = os.path.join(os.path.dirname(__file__), "option_a_side_track_compare.png")
fig.savefig(out, dpi=150, bbox_inches="tight")
print(f"wrote {out}")
