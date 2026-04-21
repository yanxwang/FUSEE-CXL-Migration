#!/usr/bin/env python3
"""
Plot throughput/latency vs write ratio for A/B/C.
Input: wr_scan_cxl.log with RESULT lines including wratio= field.
"""

import sys
import re
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_wr_scan.py <log> [<out.png>]")
    sys.exit(1)

log_path = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else "bench/wr_scan_cxl.png"

# opt -> {wratio: [result_row, ...]}
rows = defaultdict(lambda: defaultdict(list))

pat = re.compile(
    r"RESULT\s+node_id=(\d+)\s+opt=(\w)\s+wl=\w+\s+wratio=([\d.]+)\s+thpt=([\d.]+)"
    r"(?:\s+w_avg=([\d.]+)\s+w_p50=([\d.]+)\s+w_p99=([\d.]+))?"
    r"(?:\s+r_avg=([\d.]+)\s+r_p50=([\d.]+)\s+r_p99=([\d.]+))?"
)

with open(log_path) as f:
    for line in f:
        m = pat.search(line)
        if not m:
            continue
        opt = m.group(2)
        wratio = float(m.group(3))
        rows[opt][wratio].append({
            "thpt": float(m.group(4)),
            "w_avg": float(m.group(5)) if m.group(5) else None,
            "w_p99": float(m.group(7)) if m.group(7) else None,
            "r_avg": float(m.group(8)) if m.group(8) else None,
            "r_p99": float(m.group(10)) if m.group(10) else None,
        })

if not rows:
    print("no rows parsed"); sys.exit(1)

# Aggregate: sum thpt, avg latencies
agg = defaultdict(dict)
for opt, per_wr in rows.items():
    for wr, rs in per_wr.items():
        total_thpt = sum(r["thpt"] for r in rs)
        def avg(field):
            vals = [r[field] for r in rs if r[field] is not None and r[field] > 0]
            return sum(vals)/len(vals) if vals else None
        agg[opt][wr] = {
            "thpt": total_thpt,
            "w_avg": avg("w_avg"),
            "w_p99": avg("w_p99"),
            "r_avg": avg("r_avg"),
            "r_p99": avg("r_p99"),
        }

# Print summary
for opt in sorted(agg.keys()):
    print(f"=== Option {opt} ===")
    for wr in sorted(agg[opt].keys()):
        v = agg[opt][wr]
        print(f"  wr={wr:.2f} thpt={v['thpt']:>12,.0f} "
              f"w_avg={str(v['w_avg']):>10} r_avg={str(v['r_avg']):>8}")

colors = {"A": "#C62828", "B": "#E65100", "C": "#2E7D32"}
markers = {"A": "o", "B": "s", "C": "^"}

fig, axes = plt.subplots(2, 2, figsize=(13, 9))

# (1) Throughput vs write ratio
ax = axes[0][0]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted(agg[opt].keys())
    ys = [agg[opt][x]["thpt"] for x in xs]
    ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
            label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Write ratio")
ax.set_ylabel("Throughput (ops/sec, aggregate)")
ax.set_title("Throughput vs Write Ratio")
ax.legend()
ax.grid(True, alpha=0.3)
ax.set_xlim(-0.05, 1.05)
ax.set_ylim(bottom=0)

# (2) Throughput vs write ratio (log y)
ax = axes[0][1]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted(agg[opt].keys())
    ys = [agg[opt][x]["thpt"] for x in xs]
    ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
            label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Write ratio")
ax.set_ylabel("Throughput (ops/sec, log scale)")
ax.set_title("Throughput vs Write Ratio (log y)")
ax.set_yscale("log")
ax.legend()
ax.grid(True, alpha=0.3, which="both")
ax.set_xlim(-0.05, 1.05)

# (3) Write latency vs write ratio
ax = axes[1][0]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted([x for x in agg[opt].keys() if agg[opt][x]["w_avg"]])
    ys = [agg[opt][x]["w_avg"] for x in xs]
    if ys:
        ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
                label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Write ratio")
ax.set_ylabel("Write latency avg (us)")
ax.set_title("Write latency vs Write Ratio")
ax.set_yscale("log")
ax.legend()
ax.grid(True, alpha=0.3, which="both")
ax.set_xlim(-0.05, 1.05)

# (4) Read latency vs write ratio
ax = axes[1][1]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted([x for x in agg[opt].keys() if agg[opt][x]["r_avg"]])
    ys = [agg[opt][x]["r_avg"] for x in xs]
    if ys:
        ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
                label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Write ratio")
ax.set_ylabel("Read latency avg (us)")
ax.set_title("Read latency vs Write Ratio")
ax.legend()
ax.grid(True, alpha=0.3)
ax.set_xlim(-0.05, 1.05)
ax.set_ylim(bottom=0)

fig.suptitle("A/B/C vs Write Ratio  (4 procs, real CXL /dev/dax0.1 on g4)",
             fontsize=14, fontweight="bold")
plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig(out_path, dpi=150, bbox_inches="tight")
print(f"\nPlot saved to {out_path}")
