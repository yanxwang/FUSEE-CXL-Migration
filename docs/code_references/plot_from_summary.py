#!/usr/bin/env python3
"""
Plot A/B/C benchmark results from abc_summary.txt.

Input file format (produced by plot_abc.py):
    ================================================================================
    A / B / C Protocol Comparison Results
    ================================================================================
    opt wl        thpt (ops/s)   w_avg (us)   w_p99 (us)   r_avg (us)   r_p99 (us)
    --------------------------------------------------------------------------------
    A   A                7,815      1991.73      3529.07         0.23         0.90
    A   C           15,536,440           --           --         0.44         4.66
    ...

Output: bench/abc_results.png (same 4-panel layout as plot_abc.py)
"""

import sys
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) < 2:
    print("usage: plot_from_summary.py <abc_summary.txt> [<output.png>]")
    sys.exit(1)

summary_path = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else "bench/abc_results.png"


def parse_number(tok):
    """Accept '7,815', '15,536,440', '0.44', '--'."""
    if tok == "--" or tok == "":
        return 0.0
    return float(tok.replace(",", ""))


# ── Parse ──
rows = []
started = False
with open(summary_path) as f:
    for line in f:
        line = line.rstrip()
        # Find header row
        if line.startswith("opt") and "thpt" in line:
            started = True
            continue
        if not started:
            continue
        # Stop at separator or empty line
        if line.startswith("-") or line.startswith("=") or not line.strip():
            # first separator is the one between header and data; subsequent is the end
            if rows:
                break
            else:
                continue
        # Data row: "A   A    7,815    1991.73  3529.07   0.23  0.90"
        parts = line.split()
        if len(parts) < 7:
            continue
        try:
            opt = parts[0]
            wl  = parts[1]
            if opt not in ("A", "B", "C") or wl not in ("A", "C"):
                continue
            thpt  = parse_number(parts[2])
            w_avg = parse_number(parts[3])
            w_p99 = parse_number(parts[4])
            r_avg = parse_number(parts[5])
            r_p99 = parse_number(parts[6])
            rows.append({
                "opt": opt, "wl": wl, "thpt": thpt,
                "w_avg": w_avg, "w_p99": w_p99,
                "r_avg": r_avg, "r_p99": r_p99,
            })
        except ValueError:
            continue

if not rows:
    print(f"no data rows parsed from {summary_path}")
    sys.exit(1)

# Index by (opt, wl)
agg = {(r["opt"], r["wl"]): r for r in rows}

print(f"Parsed {len(rows)} rows from {summary_path}")
for r in rows:
    print(f"  opt={r['opt']} wl={r['wl']}  thpt={r['thpt']:>15,.0f}  "
          f"w_avg={r['w_avg']:>8.2f}  w_p99={r['w_p99']:>8.2f}  "
          f"r_avg={r['r_avg']:>6.2f}  r_p99={r['r_p99']:>6.2f}")

# ── Plot ──
opts = ["A", "B", "C"]
colors = {"A": "#C62828", "B": "#E65100", "C": "#2E7D32"}
wls  = ["A", "C"]

fig, axes = plt.subplots(2, 2, figsize=(13, 9))

x = np.arange(len(opts))

def fmt_thpt(v):
    if v >= 1e6: return f"{v/1e6:.2f}M"
    if v >= 1e3: return f"{v/1e3:.0f}k"
    return f"{v:.0f}"

# ── (1) Throughput — YCSB A and C grouped ──
ax = axes[0][0]
w = 0.35
thpt_A = [agg.get((o, "A"), {}).get("thpt", 0) for o in opts]
thpt_C = [agg.get((o, "C"), {}).get("thpt", 0) for o in opts]
thpt_max = max(max(thpt_A), max(thpt_C)) * 1.15

bars_A = ax.bar(x - w/2, thpt_A, w, label="YCSB A (50% write)",
                color=[colors[o] for o in opts], alpha=0.95)
bars_C = ax.bar(x + w/2, thpt_C, w, label="YCSB C (100% read)",
                color=[colors[o] for o in opts], alpha=0.55, hatch="//")

for b, v in zip(bars_A, thpt_A):
    ax.text(b.get_x() + b.get_width()/2, v + thpt_max * 0.01,
            fmt_thpt(v), ha="center", fontsize=9)
for b, v in zip(bars_C, thpt_C):
    ax.text(b.get_x() + b.get_width()/2, v + thpt_max * 0.01,
            fmt_thpt(v), ha="center", fontsize=9)

ax.set_xticks(x); ax.set_xticklabels([f"Option {o}" for o in opts])
ax.set_ylabel("Throughput (ops/sec, aggregate)")
ax.set_title("Throughput on YCSB A & C")
ax.set_ylim(0, thpt_max)
ax.legend(loc="upper left")
ax.grid(True, alpha=0.3, axis="y")

# helper: draw avg + p99 latency pair for each option
def draw_latency_panel(ax, series_avg, series_p99, title, ylabel, fmt="{:.2f}"):
    xs1 = x - 0.18; xs2 = x + 0.18
    lat_max = max(max(series_avg), max(series_p99)) * 1.15
    if lat_max <= 0: lat_max = 1
    b_a = ax.bar(xs1, series_avg, 0.32, label="avg",
                 color=[colors[o] for o in opts], alpha=0.95)
    b_p = ax.bar(xs2, series_p99, 0.32, label="p99",
                 color=[colors[o] for o in opts], alpha=0.55, hatch="//")
    for b, v in zip(b_a, series_avg):
        ax.text(b.get_x() + b.get_width()/2, v + lat_max * 0.01,
                fmt.format(v), ha="center", fontsize=9)
    for b, v in zip(b_p, series_p99):
        ax.text(b.get_x() + b.get_width()/2, v + lat_max * 0.01,
                fmt.format(v), ha="center", fontsize=9)
    ax.set_xticks(x); ax.set_xticklabels([f"Option {o}" for o in opts])
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_ylim(0, lat_max)
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3, axis="y")

# ── (2) Write latency — YCSB A ──
draw_latency_panel(
    axes[0][1],
    [agg.get((o, "A"), {}).get("w_avg", 0) for o in opts],
    [agg.get((o, "A"), {}).get("w_p99", 0) for o in opts],
    title="Write latency on YCSB A",
    ylabel="Write latency (us)",
    fmt="{:.1f}",
)

# ── (3) Read latency — YCSB A (mixed) ──
draw_latency_panel(
    axes[1][0],
    [agg.get((o, "A"), {}).get("r_avg", 0) for o in opts],
    [agg.get((o, "A"), {}).get("r_p99", 0) for o in opts],
    title="Read latency on YCSB A (mixed workload)",
    ylabel="Read latency (us)",
    fmt="{:.2f}",
)

# ── (4) Read latency — YCSB C (pure read) ──
draw_latency_panel(
    axes[1][1],
    [agg.get((o, "C"), {}).get("r_avg", 0) for o in opts],
    [agg.get((o, "C"), {}).get("r_p99", 0) for o in opts],
    title="Read latency on YCSB C (100% read)",
    ylabel="Read latency (us)",
    fmt="{:.2f}",
)

fig.suptitle("A/B/C Protocol Comparison  (YCSB Workloads A & C)",
             fontsize=14, fontweight="bold")
plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig(out_path, dpi=150, bbox_inches="tight")
print(f"\nPlot saved to {out_path}")
