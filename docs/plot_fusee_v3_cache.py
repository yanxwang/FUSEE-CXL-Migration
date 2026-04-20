#!/usr/bin/env python3
"""Plot v3 multi-proc bench results (cache-on only; cache-off rows had
transient timeouts in the sweep)."""
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOG = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    Path(__file__).parent / "fusee_mp_bench_v3.log")
OUT = Path(__file__).parent / "fusee_mp_bench_v3_cache_on.png"

HDR = re.compile(r"--- cache=\[([^\]]*)\] opt=(\w) wr=([\d.]+) ---")
HOST = re.compile(
    r"HOST opt=(\w) host=(\d+) ops=(\d+) wall=([\d.]+)s thpt=([\d.]+)\s+"
    r"w_avg=([\d.]+)\s+w_p50=([\d.]+)\s+w_p99=([\d.]+)\s+"
    r"r_avg=([\d.]+)\s+r_p50=([\d.]+)\s+r_p99=([\d.]+)")
AGG = re.compile(
    r"AGG opt=(\w) num_hosts=(\d+) wratio=([\d.]+) total_ops=(\d+)\s+"
    r"wall_max=([\d.]+)s agg_thpt=([\d.]+)")

records = []  # list of (cache_on, opt, wr, host_rows[], agg)
cur_hosts = []
cur = None
for line in LOG.read_text().splitlines():
    m = HDR.search(line)
    if m:
        if cur is not None:
            cur["hosts"] = cur_hosts
            records.append(cur)
        cur = {"cache": "FUSEE_CACHE=1" in m.group(1), "opt": m.group(2),
               "wr": float(m.group(3)), "hosts": [], "agg": None}
        cur_hosts = []
        continue
    mh = HOST.search(line)
    if mh and cur is not None:
        cur_hosts.append({
            "w_avg": float(mh.group(6)), "w_p99": float(mh.group(8)),
            "r_avg": float(mh.group(9)), "r_p99": float(mh.group(11)),
        })
        continue
    ma = AGG.search(line)
    if ma and cur is not None:
        cur["agg"] = float(ma.group(6))
if cur is not None:
    cur["hosts"] = cur_hosts
    records.append(cur)

def pick(cache_on, opt, wr):
    for r in records:
        if r["cache"] == cache_on and r["opt"] == opt and r["wr"] == wr:
            return r
    return None

def mean_field(r, f):
    if not r or not r["hosts"]: return float("nan")
    xs = [h[f] for h in r["hosts"] if h[f] > 0]
    return float(np.mean(xs)) if xs else float("nan")

opts = ["A", "B", "C"]
wrs = [0.0, 0.5, 1.0]
colors = {"A": "#d6604d", "B": "#f4a582", "C": "#4393c3"}

fig, axes = plt.subplots(2, 2, figsize=(12, 9))

# Panel 1: aggregate throughput (cache ON).
ax = axes[0, 0]
x = np.arange(len(wrs))
w = 0.25
for i, opt in enumerate(opts):
    vals = []
    for wr in wrs:
        r = pick(True, opt, wr)
        vals.append(r["agg"] / 1e3 if r and r["agg"] else 0)
    bars = ax.bar(x + (i - 1) * w, vals, w, label=f"opt {opt}",
                  color=colors[opt], edgecolor="black")
    for j, v in enumerate(vals):
        if v > 0:
            ax.text(x[j] + (i - 1) * w, v, f"{v:.0f}",
                    ha="center", va="bottom", fontsize=8)
ax.set_xticks(x)
ax.set_xticklabels([f"w={wr}" for wr in wrs])
ax.set_ylabel("Agg throughput (k ops/s, 4 hosts)")
ax.set_title("Aggregate throughput — emr /dev/dax0.0, cache ON")
ax.legend(); ax.grid(axis="y", alpha=0.3)

def lat_panel(ax, wr, field_avg, field_p99, ylabel, title):
    avgs = [mean_field(pick(True, o, wr), field_avg) for o in opts]
    p99s = [mean_field(pick(True, o, wr), field_p99) for o in opts]
    x = np.arange(len(opts))
    ax.bar(x - 0.2, avgs, 0.4, label="avg",
           color=[colors[o] for o in opts], edgecolor="black")
    ax.bar(x + 0.2, p99s, 0.4, label="p99",
           color=[colors[o] for o in opts], alpha=0.5, edgecolor="black")
    for i, (a, p) in enumerate(zip(avgs, p99s)):
        if a == a and a > 0: ax.text(i - 0.2, a, f"{a:.1f}", ha="center", va="bottom", fontsize=8)
        if p == p and p > 0: ax.text(i + 0.2, p, f"{p:.1f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x); ax.set_xticklabels([f"opt {o}" for o in opts])
    ax.set_ylabel(ylabel); ax.set_title(title)
    ax.legend(); ax.grid(axis="y", alpha=0.3)

lat_panel(axes[0, 1], 0.5, "w_avg", "w_p99",
          "Write latency (μs)", "Write latency — wr=0.5, cache ON")
lat_panel(axes[1, 0], 0.5, "r_avg", "r_p99",
          "Read latency (μs)", "Read latency — wr=0.5, cache ON")
lat_panel(axes[1, 1], 0.0, "r_avg", "r_p99",
          "Read latency (μs)", "Read latency — wr=0.0, cache ON")

plt.tight_layout()
plt.savefig(OUT, dpi=150)
print(f"wrote {OUT}")

print()
print(f"{'opt':<4}{'wr':<6}{'agg_thpt':>12}{'w_avg':>9}{'r_avg':>9}")
for opt in opts:
    for wr in wrs:
        r = pick(True, opt, wr)
        if not r or not r["agg"]: continue
        print(f"{opt:<4}{wr:<6.2f}{r['agg']:>12.0f}"
              f"{mean_field(r, 'w_avg'):>9.2f}{mean_field(r, 'r_avg'):>9.2f}")
