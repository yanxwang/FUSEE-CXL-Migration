#!/usr/bin/env python3
"""Compare 2-host cross-host scaling vs 1-host solo scaling (opt C).

Reads:
  docs/g34_scaling_ycsb/SUMMARY.log            (2-host, opt A/B/C)
  logs/g4_solo_scaling_<latest>/SUMMARY.log    (1-host, opt C only)

Emits: docs/g34_scaling_ycsb/extra/cross_host_gain.png
  One subplot per workload, showing (2-host thpt) / (1-host thpt) vs T.
  Ideal 2-host = 2x 1-host if bandwidth doubles.
"""
import os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"num_hosts=(?P<H>\d+)\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=(?P<tops>\d+)\s+trans_wall_max=(?P<twall>[\d.]+)s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)")

def parse(log, want_hosts=None, want_cache=1, want_opt="C"):
    d = {}
    with open(log) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m: continue
            if int(m.group("c")) != want_cache: continue
            if m.group("opt") != want_opt: continue
            if want_hosts is not None and int(m.group("H")) != want_hosts:
                continue
            t = TAG.search(line)
            wl = t.group("wl") if t else None
            if not wl: continue
            d[(wl, int(m.group("T")))] = float(m.group("thpt"))
    return d

xhost_log = "/home/yanwang/FUSEE/docs/g34_scaling_ycsb/SUMMARY.log"
solo_logs = sorted([p for p in os.listdir("/home/yanwang/FUSEE/logs")
                    if p.startswith("g4_solo_scaling_")])
if not solo_logs:
    sys.exit("no g4_solo_scaling_* logs found")
solo_log = f"/home/yanwang/FUSEE/logs/{solo_logs[-1]}/SUMMARY.log"
print("xhost:", xhost_log)
print("solo :", solo_log)

x2 = parse(xhost_log, want_hosts=2)
x1 = parse(solo_log, want_hosts=1)
wls = sorted({wl for (wl, _) in x2} & {wl for (wl, _) in x1})
print("workloads:", wls)

Ts = [1, 2, 4, 8, 16, 32, 64, 86]
fig, axes = plt.subplots(1, len(wls), figsize=(4.5 * len(wls), 4),
                         sharey=False)
if len(wls) == 1: axes = [axes]
for ax, wl in zip(axes, wls):
    y2 = [x2.get((wl, t), 0) / 1e3 for t in Ts]
    y1 = [x1.get((wl, t), 0) / 1e3 for t in Ts]
    ax.plot(Ts, y2, "o-", lw=2, label="2 hosts (g3+g4)", color="#3182bd")
    ax.plot(Ts, y1, "s--", lw=2, label="1 host  (g4 alone)", color="#fd8d3c")
    ax.plot(Ts, [v*2 for v in y1], ":", alpha=0.5, color="#fd8d3c",
            label="2× 1-host (ideal scaling)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("#clients per host")
    ax.set_ylabel("C agg thpt (kops/s)")
    ax.set_title(f"{wl}")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(fontsize=7, loc="upper left")

fig.suptitle("Cross-host scaling gain — opt C, cache on")
fig.tight_layout()
out = "/home/yanwang/FUSEE/docs/g34_scaling_ycsb/extra/cross_host_gain.png"
fig.savefig(out, dpi=150)
print(f"wrote {out}")

# Also compute ratio table
print("\nRatio table (2-host / 1-host):")
print(f"{'workload':<12} " + " ".join(f"T={t:>3}" for t in Ts))
for wl in wls:
    ratios = []
    for t in Ts:
        v2 = x2.get((wl, t), 0)
        v1 = x1.get((wl, t), 0)
        ratios.append(f"{v2/v1:.2f}" if v1 > 0 else "  — ")
    print(f"{wl:<12} " + " ".join(f"{r:>5}" for r in ratios))
