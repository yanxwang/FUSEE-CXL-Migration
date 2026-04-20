#!/usr/bin/env python3
"""Parse a FUSEE-port multi-proc bench log and make a 2x2 comparison plot.

Lines like:
  HOST opt=A host=0 ops=2000 wall=0.123s thpt=16234 w_avg=6.10 w_p50=... w_p99=... r_avg=... r_p50=... r_p99=...
  AGG  opt=A num_hosts=4 wratio=0.50 total_ops=8000 wall_max=0.150s agg_thpt=53333

Panels:
  (0,0) aggregate throughput by (opt, wratio)
  (0,1) write latency avg+p99 at wratio=0.50 (YCSB-A style)
  (1,0) read latency avg+p99 at wratio=0.50
  (1,1) read latency avg+p99 at wratio=0.00 (YCSB-C style)
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOG = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    Path(__file__).parent / "fusee_mp_bench.log")
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else (
    Path(__file__).parent / "fusee_mp_bench.png")

HOST_PAT = re.compile(
    r"HOST opt=(\w) host=(\d+) ops=(\d+) wall=([\d.]+)s thpt=([\d.]+)\s+"
    r"w_avg=([\d.]+)\s+w_p50=([\d.]+)\s+w_p99=([\d.]+)\s+"
    r"r_avg=([\d.]+)\s+r_p50=([\d.]+)\s+r_p99=([\d.]+)"
)
AGG_PAT = re.compile(
    r"AGG opt=(\w) num_hosts=(\d+) wratio=([\d.]+) total_ops=(\d+)\s+"
    r"wall_max=([\d.]+)s agg_thpt=([\d.]+)"
)

host_rows = []
agg_rows = []
lines = LOG.read_text().splitlines()
# Two-pass: collect AGG first so HOST rows preceding each AGG can inherit its
# wratio (the HOST lines are emitted before the AGG line that summarizes them).
pending_hosts = []
for line in lines:
    ma = AGG_PAT.search(line)
    if ma:
        wr = float(ma.group(3))
        agg_rows.append({
            "opt": ma.group(1), "nh": int(ma.group(2)),
            "wr": wr, "ops": int(ma.group(4)),
            "wall": float(ma.group(5)), "agg_thpt": float(ma.group(6)),
        })
        # Flush pending HOSTs for this block with the now-known wratio.
        for h in pending_hosts:
            h["wr"] = wr
        host_rows.extend(pending_hosts)
        pending_hosts = []
        continue
    mh = HOST_PAT.search(line)
    if mh:
        pending_hosts.append({
            "opt": mh.group(1), "host": int(mh.group(2)),
            "ops": int(mh.group(3)), "wall": float(mh.group(4)),
            "thpt": float(mh.group(5)),
            "w_avg": float(mh.group(6)), "w_p50": float(mh.group(7)),
            "w_p99": float(mh.group(8)),
            "r_avg": float(mh.group(9)), "r_p50": float(mh.group(10)),
            "r_p99": float(mh.group(11)),
            "wr":    None,
        })

if not agg_rows:
    print("no AGG lines parsed from", LOG)
    sys.exit(1)

opts = ["A", "B", "C"]
wratios = sorted({r["wr"] for r in agg_rows})
colors = {"A": "#d6604d", "B": "#f4a582", "C": "#4393c3"}

def mean(xs):
    xs = [x for x in xs if x == x and x > 0]  # drop NaN/zero
    return float(np.mean(xs)) if xs else float("nan")

fig, axes = plt.subplots(2, 2, figsize=(11, 8))

# Panel (0,0): agg throughput by wratio.
ax = axes[0, 0]
x = np.arange(len(wratios))
w = 0.25
for i, opt in enumerate(opts):
    vals = []
    for wr in wratios:
        matches = [r["agg_thpt"] for r in agg_rows if r["opt"] == opt and r["wr"] == wr]
        vals.append(matches[0] if matches else float("nan"))
    ax.bar(x + (i - 1) * w, [v / 1e3 for v in vals], w, label=f"opt {opt}",
           color=colors[opt], edgecolor="black")
    for j, v in enumerate(vals):
        if v == v:
            ax.text(x[j] + (i - 1) * w, v / 1e3, f"{v/1e3:.0f}",
                    ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"w={wr}" for wr in wratios])
ax.set_ylabel("Agg throughput (k ops/s)")
ax.set_title("Aggregate throughput — emr /dev/dax0.0, 4 hosts")
ax.legend()
ax.grid(axis="y", alpha=0.3)

def panel_latency(ax, title, wr, field_avg, field_p99, ylabel):
    vals_avg = [mean([r[field_avg] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
                for opt in opts]
    vals_p99 = [mean([r[field_p99] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
                for opt in opts]
    x = np.arange(len(opts))
    ax.bar(x - 0.2, vals_avg, 0.4, label="avg",
           color=[colors[o] for o in opts], edgecolor="black")
    ax.bar(x + 0.2, vals_p99, 0.4, label="p99",
           color=[colors[o] for o in opts], alpha=0.5, edgecolor="black")
    for i, (a, p) in enumerate(zip(vals_avg, vals_p99)):
        if a == a: ax.text(i - 0.2, a, f"{a:.1f}", ha="center", va="bottom", fontsize=8)
        if p == p: ax.text(i + 0.2, p, f"{p:.1f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x); ax.set_xticklabels([f"opt {o}" for o in opts])
    ax.set_ylabel(ylabel); ax.set_title(title)
    ax.legend(); ax.grid(axis="y", alpha=0.3)

panel_latency(axes[0, 1], "Write latency — wratio=0.50 (YCSB A style)",
              0.5, "w_avg", "w_p99", "Write latency (μs)")
panel_latency(axes[1, 0], "Read latency — wratio=0.50",
              0.5, "r_avg", "r_p99", "Read latency (μs)")
panel_latency(axes[1, 1], "Read latency — wratio=0.00 (YCSB C style)",
              0.0, "r_avg", "r_p99", "Read latency (μs)")

plt.tight_layout()
plt.savefig(OUT, dpi=150)
print(f"wrote {OUT}")

# Compact table for the doc.
print()
print(f"{'opt':<4}{'wr':<6}{'agg_thpt':>12}{'w_avg':>9}{'w_p99':>9}{'r_avg':>9}{'r_p99':>9}")
for opt in opts:
    for wr in wratios:
        agg = [r["agg_thpt"] for r in agg_rows if r["opt"] == opt and r["wr"] == wr]
        if not agg: continue
        w_a = mean([r["w_avg"] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
        w_p = mean([r["w_p99"] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
        r_a = mean([r["r_avg"] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
        r_p = mean([r["r_p99"] for r in host_rows if r["opt"] == opt and r["wr"] == wr])
        print(f"{opt:<4}{wr:<6.2f}{agg[0]:>12.0f}"
              f"{w_a:>9.2f}{w_p:>9.2f}{r_a:>9.2f}{r_p:>9.2f}")
