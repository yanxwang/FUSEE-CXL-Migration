#!/usr/bin/env python3
"""Plot thread-scaling of A/B/C on real CXL."""

import sys, re
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

log = sys.argv[1] if len(sys.argv) > 1 else "bench/thread_scan_cxl.log"
out = sys.argv[2] if len(sys.argv) > 2 else "bench/thread_scan.png"

# "[node 0 opt=A wratio=0.50 threads=1] total=..."
node_line = re.compile(r"\[node\s+\d+\s+opt=(\w)\s+\S+\s+threads=(\d+)\]")
res = re.compile(
    r"RESULT\s+node_id=\d+\s+opt=(\w)\s+wl=\w+\s+wratio=[\d.]+\s+thpt=([\d.]+)"
    r"(?:\s+w_avg=([\d.]+))?"
    r"(?:\s+.*r_avg=([\d.]+))?"
)

cur_opt, cur_threads = None, None
data = defaultdict(lambda: defaultdict(list))

with open(log) as f:
    for line in f:
        m = node_line.search(line)
        if m:
            cur_opt, cur_threads = m.group(1), int(m.group(2))
            continue
        m = res.search(line)
        if m and cur_opt:
            data[cur_opt][cur_threads].append({
                "thpt": float(m.group(2)),
                "w_avg": float(m.group(3)) if m.group(3) else None,
            })

# Aggregate
agg = defaultdict(dict)
for opt in data:
    for t, rs in data[opt].items():
        agg[opt][t] = {
            "thpt": sum(r["thpt"] for r in rs),
            "w_avg": sum(r["w_avg"] for r in rs if r["w_avg"])/max(1,len([r for r in rs if r["w_avg"]])),
        }

for opt in sorted(agg):
    print(f"=== Option {opt} ===")
    for t in sorted(agg[opt]):
        v = agg[opt][t]
        print(f"  threads={t} thpt={v['thpt']:>12,.0f} w_avg={v['w_avg']:.2f}")

colors = {"A": "#C62828", "B": "#E65100", "C": "#2E7D32"}
markers = {"A": "o", "B": "s", "C": "^"}

fig, axes = plt.subplots(1, 2, figsize=(13, 5))

ax = axes[0]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted(agg[opt].keys())
    ys = [agg[opt][x]["thpt"] for x in xs]
    ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
            label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Threads per node")
ax.set_ylabel("Throughput (ops/sec, aggregate)")
ax.set_title("YCSB A Throughput vs Threads (log y)")
ax.set_yscale("log")
ax.grid(True, alpha=0.3, which="both")
ax.legend()

ax = axes[1]
for opt in ["A", "B", "C"]:
    if opt not in agg: continue
    xs = sorted(agg[opt].keys())
    ys = [agg[opt][x]["w_avg"] for x in xs]
    ax.plot(xs, ys, marker=markers[opt], color=colors[opt],
            label=f"Option {opt}", linewidth=2, markersize=8)
ax.set_xlabel("Threads per node")
ax.set_ylabel("Write latency avg (us)")
ax.set_title("YCSB A Write Latency vs Threads (log y)")
ax.set_yscale("log")
ax.grid(True, alpha=0.3, which="both")
ax.legend()

fig.suptitle("A/B/C Thread Scaling  (2 nodes, real CXL /dev/dax0.1 on g4)",
             fontsize=13, fontweight="bold")
plt.tight_layout(rect=[0,0,1,0.95])
plt.savefig(out, dpi=150, bbox_inches="tight")
print(f"Saved {out}")
