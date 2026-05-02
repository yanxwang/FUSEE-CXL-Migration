#!/usr/bin/env python3
"""Parse the g3 tmpfs ycsb_abc_bench log and produce a 2x2 comparison plot.

Input: docs/ycsb_abc_g3_tmpfs.log (lines of the form:
  RESULT node_id=N opt=X wl=Y wratio=R thpt=T [w_avg=... w_p50=... w_p99=...] r_avg=... r_p50=... r_p99=...)

Two workloads: A (50/50 read/update) and C (100% read).
Three protocols: A (A-v2 SPSC ring), B (eager push), C (lazy RC).
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOG = Path(__file__).parent / "ycsb_abc_g3_tmpfs.log"
OUT = Path(__file__).parent / "ycsb_abc_g3_tmpfs.png"

PAT = re.compile(
    r"RESULT node_id=(\d+) opt=(\w) wl=(\w) wratio=([\d.]+) thpt=(\d+)"
    r"(?:\s+w_avg=([\d.]+)\s+w_p50=([\d.]+)\s+w_p99=([\d.]+))?"
    r"(?:\s+r_avg=([\d.]+)\s+r_p50=([\d.]+)\s+r_p99=([\d.]+))?"
)

rows = []
for line in LOG.read_text().splitlines():
    m = PAT.search(line)
    if not m:
        continue
    rows.append({
        "node": int(m.group(1)),
        "opt":  m.group(2),
        "wl":   m.group(3),
        "wratio": float(m.group(4)),
        "thpt": int(m.group(5)),
        "w_avg": float(m.group(6)) if m.group(6) else float("nan"),
        "w_p99": float(m.group(8)) if m.group(8) else float("nan"),
        "r_avg": float(m.group(9)) if m.group(9) else float("nan"),
        "r_p99": float(m.group(11)) if m.group(11) else float("nan"),
    })

if not rows:
    print("no RESULT lines parsed")
    sys.exit(1)

# Aggregate across nodes (average, since 4 procs share the same system).
agg = defaultdict(lambda: {"thpt": [], "w_avg": [], "w_p99": [], "r_avg": [], "r_p99": []})
for r in rows:
    k = (r["opt"], r["wl"])
    agg[k]["thpt"].append(r["thpt"])
    agg[k]["w_avg"].append(r["w_avg"])
    agg[k]["w_p99"].append(r["w_p99"])
    agg[k]["r_avg"].append(r["r_avg"])
    agg[k]["r_p99"].append(r["r_p99"])

def mean(xs):
    xs = [x for x in xs if not np.isnan(x)]
    return float(np.mean(xs)) if xs else float("nan")

def sum_(xs):
    return float(np.sum(xs)) if xs else 0.0

opts = ["A", "B", "C"]
wls = ["A", "C"]
colors = {"A": "#d6604d", "B": "#f4a582", "C": "#4393c3"}

fig, axes = plt.subplots(2, 2, figsize=(11, 8))

# Panel 1 (top-left): aggregate throughput per protocol, one bar-group per workload.
ax = axes[0, 0]
x = np.arange(len(wls))
w = 0.25
for i, opt in enumerate(opts):
    vals = [sum_(agg.get((opt, wl), {}).get("thpt", [])) for wl in wls]
    ax.bar(x + (i - 1) * w, [v / 1e6 for v in vals], w, label=f"opt {opt}",
           color=colors[opt], edgecolor="black")
    for j, v in enumerate(vals):
        if v > 0:
            ax.text(x[j] + (i - 1) * w, v / 1e6, f"{v/1e6:.2f}",
                    ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"wl={wl}" for wl in wls])
ax.set_ylabel("Aggregate throughput (M ops/s, 4 procs)")
ax.set_title("Throughput — g3 tmpfs, 4 proc × 2 threads × 3000 ops")
ax.legend()
ax.grid(axis="y", alpha=0.3)

# Panel 2 (top-right): write latency (avg + p99) for workload A.
ax = axes[0, 1]
x = np.arange(len(opts))
w_avg = [mean(agg.get((opt, "A"), {}).get("w_avg", [])) for opt in opts]
w_p99 = [mean(agg.get((opt, "A"), {}).get("w_p99", [])) for opt in opts]
ax.bar(x - 0.2, w_avg, 0.4, label="avg",
       color=[colors[o] for o in opts], edgecolor="black")
ax.bar(x + 0.2, w_p99, 0.4, label="p99",
       color=[colors[o] for o in opts], alpha=0.5, edgecolor="black")
for i, (a, p) in enumerate(zip(w_avg, w_p99)):
    if not np.isnan(a):
        ax.text(i - 0.2, a, f"{a:.1f}", ha="center", va="bottom", fontsize=8)
    if not np.isnan(p):
        ax.text(i + 0.2, p, f"{p:.1f}", ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"opt {o}" for o in opts])
ax.set_ylabel("Write latency (μs)")
ax.set_title("Write latency — workload A (50/50 read/update)")
ax.legend()
ax.grid(axis="y", alpha=0.3)

# Panel 3 (bottom-left): read latency (avg + p99) for workload A.
ax = axes[1, 0]
r_avg_A = [mean(agg.get((opt, "A"), {}).get("r_avg", [])) for opt in opts]
r_p99_A = [mean(agg.get((opt, "A"), {}).get("r_p99", [])) for opt in opts]
ax.bar(x - 0.2, r_avg_A, 0.4, label="avg",
       color=[colors[o] for o in opts], edgecolor="black")
ax.bar(x + 0.2, r_p99_A, 0.4, label="p99",
       color=[colors[o] for o in opts], alpha=0.5, edgecolor="black")
for i, (a, p) in enumerate(zip(r_avg_A, r_p99_A)):
    if not np.isnan(a):
        ax.text(i - 0.2, a, f"{a:.1f}", ha="center", va="bottom", fontsize=8)
    if not np.isnan(p):
        ax.text(i + 0.2, p, f"{p:.1f}", ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"opt {o}" for o in opts])
ax.set_ylabel("Read latency (μs)")
ax.set_title("Read latency — workload A (50/50 read/update)")
ax.legend()
ax.grid(axis="y", alpha=0.3)

# Panel 4 (bottom-right): read latency for workload C (100% read).
ax = axes[1, 1]
r_avg_C = [mean(agg.get((opt, "C"), {}).get("r_avg", [])) for opt in opts]
r_p99_C = [mean(agg.get((opt, "C"), {}).get("r_p99", [])) for opt in opts]
ax.bar(x - 0.2, r_avg_C, 0.4, label="avg",
       color=[colors[o] for o in opts], edgecolor="black")
ax.bar(x + 0.2, r_p99_C, 0.4, label="p99",
       color=[colors[o] for o in opts], alpha=0.5, edgecolor="black")
for i, (a, p) in enumerate(zip(r_avg_C, r_p99_C)):
    if not np.isnan(a):
        ax.text(i - 0.2, a, f"{a:.1f}", ha="center", va="bottom", fontsize=8)
    if not np.isnan(p):
        ax.text(i + 0.2, p, f"{p:.1f}", ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"opt {o}" for o in opts])
ax.set_ylabel("Read latency (μs)")
ax.set_title("Read latency — workload C (100% read)")
ax.legend()
ax.grid(axis="y", alpha=0.3)

plt.tight_layout()
plt.savefig(OUT, dpi=150)
print(f"wrote {OUT}")

# Also dump a compact table for the side-track doc.
print()
print(f"{'opt':<4}{'wl':<4}{'thpt_sum':>12}{'w_avg':>9}{'w_p99':>9}{'r_avg':>9}{'r_p99':>9}")
for opt in opts:
    for wl in wls:
        a = agg.get((opt, wl))
        if not a:
            continue
        print(f"{opt:<4}{wl:<4}{sum_(a['thpt']):>12.0f}"
              f"{mean(a['w_avg']):>9.2f}{mean(a['w_p99']):>9.2f}"
              f"{mean(a['r_avg']):>9.2f}{mean(a['r_p99']):>9.2f}")
