#!/usr/bin/env python3
"""Plot FUSEE Figure 10-style CDFs for Protocol F microbench.

Reads results/{insert,update,search,delete}_lat-Fp.txt (one µs latency
per line, output of cxl_kv_ops_F_microbench) and produces a 2x2 subplot
mirroring FUSEE paper Figure 10:
  (a) INSERT  (b) UPDATE  (c) SEARCH  (d) DELETE

Usage:
  python3 scripts/plot_protocol_F_microbench_cdf.py <results_dir>
Output: <results_dir>/microbench_cdf.png
        <results_dir>/microbench_summary.csv (count, avg, p50, p99, p999)
"""
import csv, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_microbench_cdf.py <results_dir>")
    sys.exit(1)
RDIR = sys.argv[1]

OPS = [
    ("insert", "(a) INSERT latency CDF",  "tab:red"),
    ("update", "(b) UPDATE latency CDF",  "tab:orange"),
    ("search", "(c) SEARCH latency CDF",  "tab:blue"),
    ("delete", "(d) DELETE latency CDF",  "tab:green"),
]

def load(path):
    vals = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: vals.append(int(line))
            except ValueError: pass
    return sorted(vals)

def pct(sorted_vals, p):
    if not sorted_vals: return 0
    idx = max(0, min(len(sorted_vals) - 1, int(p * (len(sorted_vals) - 1))))
    return sorted_vals[idx]

# Load all
data = {}
for op, _, _ in OPS:
    path = os.path.join(RDIR, f"{op}_lat-Fp.txt")
    if not os.path.exists(path):
        print(f"missing {path}")
        sys.exit(1)
    data[op] = load(path)

# CSV summary
csv_path = os.path.join(RDIR, "microbench_summary.csv")
with open(csv_path, "w") as f:
    w = csv.writer(f)
    w.writerow(["op", "count", "avg_us", "p50_us", "p90_us", "p99_us",
                "p999_us", "min_us", "max_us"])
    for op, _, _ in OPS:
        v = data[op]
        w.writerow([op, len(v),
                    sum(v)/len(v) if v else 0,
                    pct(v, 0.50), pct(v, 0.90), pct(v, 0.99),
                    pct(v, 0.999),
                    v[0] if v else 0, v[-1] if v else 0])
print(f"wrote {csv_path}")

# CDF plot — matches FUSEE Fig 10 (2x2 subplots)
fig, axes = plt.subplots(2, 2, figsize=(12, 8))
for ax, (op, title, color) in zip(axes.flat, OPS):
    v = data[op]
    if not v: continue
    # build CDF
    n = len(v)
    ys = [(i + 1) / n for i in range(n)]
    ax.plot(v, ys, linewidth=2, color=color, label="FUSEE CXL Migration (LFM based)")
    # annotation: median + p99
    p50, p99 = pct(v, 0.50), pct(v, 0.99)
    ax.axvline(p50, color=color, alpha=0.3, linestyle=":")
    ax.axvline(p99, color=color, alpha=0.5, linestyle="--")
    ax.annotate(f"p50={p50}µs", xy=(p50, 0.50),
                xytext=(p50 * 1.5, 0.45), color=color, fontsize=9,
                arrowprops=dict(arrowstyle="-", color=color, alpha=0.3))
    ax.annotate(f"p99={p99}µs", xy=(p99, 0.99),
                xytext=(p99 * 1.5, 0.95), color=color, fontsize=9,
                arrowprops=dict(arrowstyle="-", color=color, alpha=0.5))
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.set_ylim(0, 1.02)
    # FUSEE Fig 10 x-axis: 0-100 µs for INSERT/UPDATE, 0-50 µs for SEARCH/DELETE
    xmax = max(v[int(0.999 * (n - 1))] * 1.2, 5)
    ax.set_xlim(0, xmax)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9, loc="lower right")

fig.suptitle("FUSEE CXL Migration (LFM based) — single-client latency CDFs "
             "(FUSEE Fig. 10 counterpart)", fontsize=12, y=1.01)
fig.tight_layout()
out = os.path.join(RDIR, "microbench_cdf.png")
fig.savefig(out, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {out}")
