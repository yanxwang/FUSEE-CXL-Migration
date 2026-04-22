#!/usr/bin/env python3
"""Plot thread scaling from docs/g34_bench/thread_scaling_g4.log.

Parses AGG lines grouped by wr, emits:
  (1) agg_thpt vs threads (three curves: wr=0/0.5/1)
  (2) w_avg + r_avg latency vs threads (same curves)
"""
import re, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LOG = sys.argv[1] if len(sys.argv) > 1 else (
    os.path.dirname(__file__) + "/g34_bench/thread_scaling_g4.log")
OUT = os.path.splitext(LOG)[0] + ".png"

HDR = re.compile(r"---\s+wr=([\d.]+)\s+threads=(\d+)\s+---")
AGG = re.compile(r"^AGG threads=(\d+) wratio=([\d.]+) total_ops=\d+ wall_max=([\d.]+)s wall_total=[\d.]+s agg_thpt=([\d.]+)")
TH  = re.compile(r"^THREAD tid=(\d+).*w_avg=([\d.]+) w_p99=([\d.]+) r_avg=([\d.]+) r_p99=([\d.]+)")

# wr -> threads -> {thpt, w_avgs[], r_avgs[]}
data = {}
cur_wr, cur_n = None, None
with open(LOG) as fh:
    for line in fh:
        m = HDR.search(line)
        if m:
            cur_wr = float(m.group(1)); cur_n = int(m.group(2))
            data.setdefault(cur_wr, {}).setdefault(cur_n, {"thpt":0,"w_avg":[],"r_avg":[]})
            continue
        m = AGG.search(line)
        if m:
            data[float(m.group(2))][int(m.group(1))]["thpt"] = float(m.group(4))
            continue
        m = TH.search(line)
        if m:
            d = data[cur_wr][cur_n]
            if float(m.group(2)) > 0: d["w_avg"].append(float(m.group(2)))
            if float(m.group(4)) > 0: d["r_avg"].append(float(m.group(4)))

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))

colors = {0.0: "#31a354", 0.5: "#e6550d", 1.0: "#3182bd"}
wrs = sorted(data.keys())
for wr in wrs:
    ns = sorted(data[wr].keys())
    thpts = [data[wr][n]["thpt"] / 1e3 for n in ns]
    label = {0.0:"read-only", 0.5:"50/50", 1.0:"write-only"}[wr]
    ax1.plot(ns, thpts, "o-", color=colors[wr],
             label=f"wr={wr} ({label})", lw=2, markersize=7)
ax1.set_xscale("log", base=2)
ax1.set_yscale("log")
ax1.set_xticks([1,2,4,8,16]); ax1.set_xticklabels(["1","2","4","8","16"])
ax1.set_xlabel("threads (single-process pthreads)")
ax1.set_ylabel("agg throughput (kops/s)")
ax1.set_title("Thread scaling on g4 /dev/dax0.0\n(C semantics, 65 536 buckets, 5 000 ops/thread)")
# Linear reference line for each wr
for wr in wrs:
    ns = sorted(data[wr].keys())
    if not ns: continue
    base = data[wr][ns[0]]["thpt"] / 1e3
    ax1.plot(ns, [base * n for n in ns], "--", color=colors[wr],
             alpha=0.3, label=f"ideal (wr={wr})")
ax1.grid(True, alpha=0.3, which="both"); ax1.legend(loc="best", fontsize=8)

# Latency panel
for wr in wrs:
    ns = sorted(data[wr].keys())
    if wr > 0:
        w_means = [(sum(data[wr][n]["w_avg"]) / len(data[wr][n]["w_avg"])) if data[wr][n]["w_avg"] else 0 for n in ns]
        ax2.plot(ns, w_means, "s-", color=colors[wr],
                 label=f"wr={wr} write", lw=2, markersize=6)
    if wr < 1:
        r_means = [(sum(data[wr][n]["r_avg"]) / len(data[wr][n]["r_avg"])) if data[wr][n]["r_avg"] else 0 for n in ns]
        ax2.plot(ns, r_means, "^--", color=colors[wr],
                 label=f"wr={wr} read", lw=2, markersize=6)
ax2.set_xscale("log", base=2)
ax2.set_xticks([1,2,4,8,16]); ax2.set_xticklabels(["1","2","4","8","16"])
ax2.set_xlabel("threads")
ax2.set_ylabel("avg latency per op (μs)")
ax2.set_title("Latency vs threads (mean across threads)")
ax2.grid(True, alpha=0.3); ax2.legend(loc="best", fontsize=8)

fig.tight_layout()
fig.savefig(OUT, dpi=150)
print(f"wrote {OUT}")
