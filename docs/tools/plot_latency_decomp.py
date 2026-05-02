#!/usr/bin/env python3
"""Plot per-primitive latency from tests/cxl_latency_decomp at N=1,2,4.

Reads docs/g34_bench/latency_decomp_g3.log (multiple '### N=k ###' blocks)
and produces a stacked bar chart of P1/P3/P4/P5/P6 and a separate P7 line.
"""
import re, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOG = sys.argv[1] if len(sys.argv) > 1 else (
    os.path.dirname(__file__) + "/g34_bench/latency_decomp_g3.log")
OUT = os.path.splitext(LOG)[0] + ".png"

HDR = re.compile(r"### N=(\d+) ###")
P   = re.compile(r"^(P[^\s]+ \S+)\s+n=\d+.*avg=(\d+)", re.MULTILINE)

runs = {}  # N -> {label: avg_ns}
cur = None
blocks = re.split(r"### N=(\d+) ###", open(LOG).read())
# blocks = [before_first, "1", block_1_text, "2", block_2_text, ...]
for i in range(1, len(blocks), 2):
    n = int(blocks[i])
    body = blocks[i+1]
    d = {}
    for m in P.finditer(body):
        label = m.group(1).strip()
        d[label] = int(m.group(2))
    runs[n] = d

Ns = sorted(runs.keys())
stacks = [("P1/2 lock_acquire", "#3182bd"),
          ("P3 flush_plus_fence", "#fdae6b"),
          ("P4 store_plus_flush", "#a1d99b"),
          ("P5 epoch_bump", "#bcbddc"),
          ("P6 unlock", "#fb6a4a")]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5),
                               gridspec_kw={"width_ratios": [3, 2]})

bar_w = 0.5
x = np.arange(len(Ns))
bottom = np.zeros(len(Ns))
for label, color in stacks:
    vals = np.array([runs[n].get(label, 0) / 1000.0 for n in Ns])
    ax1.bar(x, vals, bar_w, bottom=bottom, color=color,
            edgecolor="black", linewidth=0.3, label=label)
    bottom += vals

ax1.set_xticks(x); ax1.set_xticklabels([f"N={n}" for n in Ns])
ax1.set_ylabel("avg latency (μs)")
ax1.set_title("C-protocol write path: per-primitive latency\n(on g3 /dev/dax0.0, PCIe-switched CXL)")
ax1.legend(loc="upper left", fontsize=8)
ax1.grid(True, axis="y", alpha=0.3)

for i, n in enumerate(Ns):
    total = bottom[i]
    ax1.text(x[i], total + 1, f"{total:.1f} μs\n(~{1e6/total:.0f}k ops/s)",
             ha="center", fontsize=9)

# P7 search side panel
p7 = [runs[n].get("P7 search_optimistic", 0) / 1000.0 for n in Ns]
ax2.bar(x, p7, bar_w, color="#756bb1", edgecolor="black", linewidth=0.3)
ax2.set_xticks(x); ax2.set_xticklabels([f"N={n}" for n in Ns])
ax2.set_ylabel("avg latency (μs)")
ax2.set_title("Optimistic search (no lock)\nflush+fence+read 7 slots")
ax2.grid(True, axis="y", alpha=0.3)
for i, n in enumerate(Ns):
    ax2.text(x[i], p7[i] + 0.05, f"{p7[i]:.2f} μs", ha="center", fontsize=9)

fig.tight_layout()
fig.savefig(OUT, dpi=150)
print(f"wrote {OUT}")
