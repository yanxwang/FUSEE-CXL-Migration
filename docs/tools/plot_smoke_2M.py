#!/usr/bin/env python3
"""Two plots comparing 2M-ops smoke vs 200k-ops main sweep for Opt C on
workloadc / workloadd. Shows the per-run-size discrepancy documented in
commit de8d5ea.

Output: docs/g34_scaling_ycsb/extra/smoke_2M_vs_200k_<wl>.png (×2).
"""
import os, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = "/home/yanwang/FUSEE/docs/g34_scaling_ycsb"
MAIN = f"{BASE}/SUMMARY.log"              # 200k ops
SMOKE = f"{BASE}/extra/smoke_2M_C_cd.log"  # 2M ops
OUT   = f"{BASE}/extra"

YCSB = re.compile(
    r"opt=(?P<opt>[ABC]) cache=(?P<c>[01]) num_hosts=(?P<H>\d+) threads=(?P<T>\d+)"
    r".*?trans_agg_thpt=(?P<thpt>[\d.]+).*?r_avg_ns=(?P<ravg>\d+).*?r_p99_ns=(?P<rp99>\d+)"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)")

def parse(path, want_opt="C", want_cache=1):
    d = {}
    with open(path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m: continue
            if m.group("opt") != want_opt: continue
            if int(m.group("c")) != want_cache: continue
            t = TAG.search(line)
            wl = t.group("wl") if t else None
            if not wl: continue
            d[(wl, int(m.group("T")))] = {
                "thpt": float(m.group("thpt")),
                "r_avg": float(m.group("ravg")) / 1000.0,
                "r_p99": float(m.group("rp99")) / 1000.0,
            }
    return d

main = parse(MAIN)
smoke = parse(SMOKE)
Ts = [1, 2, 4, 8, 16, 32, 64, 86]

for wl in ("workloadc", "workloadd"):
    fig, (ax_t, ax_l) = plt.subplots(1, 2, figsize=(12, 4.5))

    y_main  = [main.get((wl, t), {}).get("thpt", 0) / 1e6 for t in Ts]
    y_smoke = [smoke.get((wl, t), {}).get("thpt", 0) / 1e6 for t in Ts]

    ax_t.plot(Ts, y_main,  "s-",  lw=2, markersize=7,
              color="#6baed6", label="200 k trans ops (original sweep)")
    ax_t.plot(Ts, y_smoke, "o-",  lw=2, markersize=7,
              color="#3182bd", label="2 M trans ops (sustained)")
    ax_t.set_xscale("log", base=2)
    ax_t.set_xticks(Ts); ax_t.set_xticklabels([str(t) for t in Ts])
    ax_t.set_xlabel("#clients per host")
    ax_t.set_ylabel("Throughput (Mops/s)")
    ax_t.set_title(f"Opt C — {wl} — 200 k vs 2 M ops (cache on)")
    ax_t.legend(); ax_t.grid(True, alpha=0.3)
    # Annotate T=86 points
    if y_main[-1] > 0:
        ax_t.annotate(f"{y_main[-1]:.1f} M", xy=(Ts[-1], y_main[-1]),
                      xytext=(-30, -18), textcoords="offset points",
                      fontsize=9, color="#6baed6")
    if y_smoke[-1] > 0:
        ax_t.annotate(f"{y_smoke[-1]:.1f} M", xy=(Ts[-1], y_smoke[-1]),
                      xytext=(-35, 6), textcoords="offset points",
                      fontsize=9, color="#3182bd", weight="bold")

    r_main  = [main.get((wl, t), {}).get("r_avg",  0) for t in Ts]
    r_smoke = [smoke.get((wl, t), {}).get("r_avg", 0) for t in Ts]
    ax_l.plot(Ts, r_main,  "s--", lw=2, markersize=6,
              color="#fdae6b", label="200 k r_avg")
    ax_l.plot(Ts, r_smoke, "o-",  lw=2, markersize=6,
              color="#e6550d", label="2 M r_avg")
    ax_l.set_xscale("log", base=2)
    ax_l.set_xticks(Ts); ax_l.set_xticklabels([str(t) for t in Ts])
    ax_l.set_xlabel("#clients per host")
    ax_l.set_ylabel("read latency (μs)")
    ax_l.set_title("per-op read avg latency")
    ax_l.legend(); ax_l.grid(True, alpha=0.3)

    fig.tight_layout()
    out = f"{OUT}/smoke_2M_vs_200k_{wl}.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"wrote {out}")
