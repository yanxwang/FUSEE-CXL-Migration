#!/usr/bin/env python3
"""Render iter-4A-redo scaling_ycsb plots per docs/scaling_ycsb_spec.md §6/§7.

Usage:
  python3 docs/tools/plot_iter4A_redo.py docs/g34_scaling_ycsb_<ts>/

Generates per workload:
  A_thpt_workload<wl>.png            # 5-rep median throughput line
  A_thpt_workload<wl>_band.png       # same with min/max shaded band
  A_lat_workload<wl>_read.png        # avg/p50/p99 read latency bars
  A_lat_workload<wl>_write.png       # avg/p50/p99 write latency bars

Plus mirror under cache_off/ for the cache=off subset.

Plus extra/:
  A_target_workload<wl>.png          # throughput vs 20 Mops/s target line
  A_scaling_efficiency.png           # peak T thpt / single-T thpt across workloads
"""
from __future__ import annotations

import os
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_style import (
    apply_style, COLORS, STYLE_B, STYLE_B_EDGE, STYLE_B_EDGE_LW,
    STYLE_B_TARGET, bar_with_headroom,
)

apply_style()

LINE = re.compile(
    r"YCSB opt=A cache=(\d+) num_hosts=(\d+) threads=(\d+) threads_eff=\d+ "
    r"rep=(\d+) load_ops=(\d+) load_thpt=(\d+) "
    r"trans_ops=(\d+) trans_wall_max=([\d\.]+) trans_agg_thpt=(\d+) "
    r"w_avg_ns=(\d+) w_p50_ns=(\d+) w_p99_ns=(\d+) "
    r"r_avg_ns=(\d+) r_p50_ns=(\d+) r_p99_ns=(\d+) "
    r"# (\w+)_optA_t(\d+)_cache(on|off)_rep(\d+)$"
)

T_AXIS = [1, 2, 4, 8, 16, 32, 64, 86]
WORKLOADS = ["workloada", "workloadb", "workloadc", "workloadd", "workloadf"]
TARGET_MOPS = 20.0
ACCENT = STYLE_B[2]  # red
GREY = STYLE_B[1]


def parse(path):
    by = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("FAIL"):
                continue
            m = LINE.match(line)
            if not m:
                continue
            (cache, _h, T, rep, _lo, _lt, _to, _tw, agg,
             w_avg, w_p50, w_p99, r_avg, r_p50, r_p99,
             wl, _T2, cache_str, _r2) = m.groups()
            cell = (wl, int(T), cache_str)
            by[cell].append({
                "agg": int(agg),
                "w_avg": int(w_avg),
                "w_p50": int(w_p50),
                "w_p99": int(w_p99),
                "r_avg": int(r_avg),
                "r_p50": int(r_p50),
                "r_p99": int(r_p99),
            })
    return by


def median(xs):
    return statistics.median(xs) if xs else 0


def plot_throughput(by, wl, cache, outdir, with_band=False):
    Ts, meds, mins, maxs = [], [], [], []
    for T in T_AXIS:
        runs = by.get((wl, T, cache), [])
        if not runs:
            continue
        aggs = [r["agg"] / 1e6 for r in runs]  # Mops/s
        Ts.append(T)
        meds.append(median(aggs))
        mins.append(min(aggs))
        maxs.append(max(aggs))
    if not Ts:
        return
    fig, ax = plt.subplots(figsize=(7, 4.2))
    ax.plot(Ts, meds, marker="o", color=ACCENT,
            markeredgecolor=STYLE_B_EDGE, markeredgewidth=0.8,
            linewidth=1.8, label="5-rep median")
    if with_band:
        ax.fill_between(Ts, mins, maxs, color=ACCENT, alpha=0.18,
                        label="min-max range")
    ax.set_xscale("log", base=2)
    ax.set_xticks(T_AXIS)
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.set_xlabel("# clients per host")
    ax.set_ylabel("aggregate throughput (Mops/s)")
    ax.set_title(f"Protocol A — {wl} (cache={cache})")
    ax.grid(axis="y", alpha=0.3, zorder=0)
    ax.set_axisbelow(True)
    ymax = max(maxs) if with_band else max(meds)
    ax.set_ylim(0, ymax * 1.25 if ymax > 0 else 1)
    if with_band:
        ax.legend(loc="upper left", frameon=False)
    fig.tight_layout()
    suffix = "_band" if with_band else ""
    fig.savefig(outdir / f"A_thpt_{wl}{suffix}.png")
    plt.close(fig)


def plot_latency(by, wl, cache, outdir, op_kind):
    """op_kind = 'read' or 'write'."""
    if op_kind == "read":
        keys = ("r_avg", "r_p50", "r_p99")
        title_op = "READ"
    else:
        keys = ("w_avg", "w_p50", "w_p99")
        title_op = "WRITE"
        if wl == "workloadc":  # no writes
            return
    Ts = []
    avgs, p50s, p99s = [], [], []
    for T in T_AXIS:
        runs = by.get((wl, T, cache), [])
        if not runs:
            continue
        # Skip if all-zero (e.g. workload-c read-only path => write zero)
        all_zero = all(r[keys[0]] == 0 for r in runs)
        if all_zero:
            continue
        Ts.append(T)
        avgs.append(median([r[keys[0]] for r in runs]) / 1000.0)  # ns→µs
        p50s.append(median([r[keys[1]] for r in runs]) / 1000.0)
        p99s.append(median([r[keys[2]] for r in runs]) / 1000.0)
    if not Ts:
        return
    fig, ax = plt.subplots(figsize=(8, 4.2))
    n = len(Ts)
    x = list(range(n))
    w = 0.27
    ax.bar([i - w for i in x], avgs, w, label="avg",
           color=STYLE_B[0], edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW)
    ax.bar(x,                p50s, w, label="p50",
           color=STYLE_B[1], edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW)
    ax.bar([i + w for i in x], p99s, w, label="p99",
           color=ACCENT, edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW)
    ax.set_xticks(x)
    ax.set_xticklabels([str(T) for T in Ts])
    ax.set_xlabel("# clients per host")
    ax.set_ylabel("latency (µs)")
    ax.set_title(f"Protocol A — {wl} {title_op} latency (cache={cache})")
    ax.legend(loc="upper left", frameon=False)
    ax.grid(axis="y", alpha=0.3, zorder=0)
    ax.set_axisbelow(True)
    ymax = max(p99s) if p99s else 1
    ax.set_ylim(0, ymax * 1.25)
    fig.tight_layout()
    fig.savefig(outdir / f"A_lat_{wl}_{op_kind}.png")
    plt.close(fig)


def plot_target(by, wl, outdir):
    """A throughput vs 20 Mops/s line, cache=on."""
    Ts, meds = [], []
    for T in T_AXIS:
        runs = by.get((wl, T, "on"), [])
        if not runs:
            continue
        Ts.append(T)
        meds.append(median([r["agg"] / 1e6 for r in runs]))
    if not Ts:
        return
    fig, ax = plt.subplots(figsize=(7, 4.2))
    ax.plot(Ts, meds, marker="o", color=ACCENT,
            markeredgecolor=STYLE_B_EDGE, markeredgewidth=0.8,
            linewidth=1.8, label=f"{wl} (5-rep median)")
    ax.axhline(TARGET_MOPS, color=STYLE_B_TARGET, linestyle="--", linewidth=1.4,
               label=f"target = {TARGET_MOPS:.0f} Mops/s")
    peak = max(meds)
    peak_T = Ts[meds.index(peak)]
    gap = TARGET_MOPS - peak
    ax.annotate(f"peak {peak:.2f}\n({peak/TARGET_MOPS*100:.1f}% of target,\n gap {gap:+.2f})",
                xy=(peak_T, peak),
                xytext=(peak_T * 0.5, peak + (TARGET_MOPS - peak) * 0.4),
                fontsize=9, ha="center",
                arrowprops=dict(arrowstyle="->", color=STYLE_B_EDGE, lw=0.8))
    ax.set_xscale("log", base=2)
    ax.set_xticks(T_AXIS)
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.set_xlabel("# clients per host")
    ax.set_ylabel("aggregate throughput (Mops/s)")
    ax.set_title(f"Protocol A — {wl} vs 20 Mops/s target")
    ax.legend(loc="upper left", frameon=False)
    ax.grid(axis="y", alpha=0.3, zorder=0)
    ax.set_axisbelow(True)
    ax.set_ylim(0, max(TARGET_MOPS, peak) * 1.25)
    fig.tight_layout()
    fig.savefig(outdir / f"A_target_{wl}.png")
    plt.close(fig)


def plot_scaling_efficiency(by, outdir):
    """Bar chart: peak Mops/s ÷ single-T Mops/s per workload, cache=on."""
    labels, ratios, peaks, singles = [], [], [], []
    for wl in WORKLOADS:
        single = by.get((wl, 1, "on"), [])
        if not single:
            continue
        single_med = median([r["agg"] / 1e6 for r in single])
        peak_med = 0
        for T in T_AXIS:
            runs = by.get((wl, T, "on"), [])
            if not runs:
                continue
            m = median([r["agg"] / 1e6 for r in runs])
            if m > peak_med:
                peak_med = m
        if single_med > 0:
            labels.append(wl)
            ratios.append(peak_med / single_med)
            peaks.append(peak_med)
            singles.append(single_med)
    if not labels:
        return
    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = list(range(len(labels)))
    ax.bar(x, ratios, color=ACCENT, edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW)
    for i, (r, p, s) in enumerate(zip(ratios, peaks, singles)):
        ax.text(i, r + 0.5, f"{r:.1f}×\n({p:.1f}/{s:.2f})",
                ha="center", va="bottom", fontsize=9)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("scaling efficiency (peak ÷ T=1)")
    ax.set_title("Protocol A — scaling efficiency per workload (cache=on)")
    ax.grid(axis="y", alpha=0.3, zorder=0)
    ax.set_axisbelow(True)
    ax.set_ylim(0, max(ratios) * 1.30)
    fig.tight_layout()
    fig.savefig(outdir / "A_scaling_efficiency.png")
    plt.close(fig)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    base = Path(sys.argv[1])
    by = parse(base / "SUMMARY.log")
    if not by:
        print("no parsable lines in SUMMARY.log", file=sys.stderr)
        sys.exit(1)

    extra_dir = base / "extra"
    extra_dir.mkdir(exist_ok=True)
    cache_off_dir = base / "cache_off"
    cache_off_dir.mkdir(exist_ok=True)

    n_thpt, n_lat, n_target, n_eff = 0, 0, 0, 0
    for wl in WORKLOADS:
        # Cache-on plots (top level).
        plot_throughput(by, wl, "on", base, with_band=False); n_thpt += 1
        plot_throughput(by, wl, "on", base, with_band=True);  n_thpt += 1
        plot_latency(by, wl, "on", base, "read"); n_lat += 1
        if wl != "workloadc":
            plot_latency(by, wl, "on", base, "write"); n_lat += 1
        # Cache-off plots (cache_off/ subdir).
        plot_throughput(by, wl, "off", cache_off_dir, with_band=False)
        plot_throughput(by, wl, "off", cache_off_dir, with_band=True)
        plot_latency(by, wl, "off", cache_off_dir, "read")
        if wl != "workloadc":
            plot_latency(by, wl, "off", cache_off_dir, "write")
        # extra/ target plots.
        plot_target(by, wl, extra_dir); n_target += 1
    plot_scaling_efficiency(by, extra_dir); n_eff += 1
    print(f"wrote {n_thpt} thpt plots, {n_lat} latency plots, "
          f"{n_target} target-gap plots, {n_eff} scaling efficiency plot")
    print(f"output dir: {base}")


if __name__ == "__main__":
    main()
