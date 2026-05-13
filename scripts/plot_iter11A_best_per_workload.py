#!/usr/bin/env python3
"""iter-11A — best-throughput-per-workload summary plots.

Two horizontal bar charts:
  1. A_best_thpt_per_workload.png — best throughput per workload across
     all (T, cache, kv) cells
  2. A_best_lat_per_workload.png  — corresponding write_avg / read_avg
     latency from the same cells

Both use docs/tools/plot_style.py — STYLE_B + workload-coloured accent.

Run:
  python3 scripts/plot_iter11A_best_per_workload.py <sweep_dir>
"""

import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Pull the project standard
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "docs", "tools"))
from plot_style import (  # noqa: E402
    apply_style, COLORS, STYLE_B, STYLE_B_EDGE, STYLE_B_EDGE_LW,
    STYLE_B_TARGET, bar_with_headroom,
)

apply_style()

LINE = re.compile(
    r"YCSB opt=A cache=(\d+) num_hosts=\d+ threads=(\d+) threads_eff=\d+ "
    r"rep=\d+ load_ops=\d+ load_thpt=\d+ "
    r"trans_ops=\d+ trans_wall_max=[\d\.]+ trans_agg_thpt=(\d+) "
    r"w_avg_ns=(\d+) w_p50_ns=(\d+) w_p99_ns=(\d+) "
    r"r_avg_ns=(\d+) r_p50_ns=(\d+) r_p99_ns=(\d+).*"
    r"# (\w+)_optA_t\d+_cache(on|off)_rep\d+_kv(\d+)$"
)

WORKLOADS = ["workloada", "workloadb", "workloadc", "workloadd", "workloadf"]
TARGET_MOPS = 20.0


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line.rstrip())
            if not m:
                continue
            (cache_int, T, agg,
             w_avg, w_p50, w_p99,
             r_avg, r_p50, r_p99,
             wl, cache_str, kv) = m.groups()
            rows.append({
                "wl": wl, "T": int(T), "cache": cache_str, "kv": int(kv),
                "agg_mops": int(agg) / 1e6,
                "w_avg_us": int(w_avg) / 1000.0,
                "w_p99_us": int(w_p99) / 1000.0,
                "r_avg_us": int(r_avg) / 1000.0,
                "r_p99_us": int(r_p99) / 1000.0,
            })
    return rows


def best_per_workload(rows):
    by_wl = {}
    for r in rows:
        by_wl.setdefault(r["wl"], []).append(r)
    out = {}
    for wl, runs in by_wl.items():
        out[wl] = max(runs, key=lambda r: r["agg_mops"])
    return out


def plot_thpt(best, out_path):
    """Horizontal bar chart, single bar per workload."""
    fig, ax = plt.subplots(figsize=(8.5, 4.5))
    wls = WORKLOADS  # fixed order a,b,c,d,f
    values = [best[wl]["agg_mops"] for wl in wls]
    colours = [COLORS[wl] for wl in wls]
    labels = [wl.replace("workload", "") for wl in wls]  # a / b / c / d / f

    # y position top-to-bottom in workload order
    y_pos = list(range(len(wls)))[::-1]
    bars = ax.barh(y_pos, values, color=colours,
                   edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(labels, fontsize=12)
    ax.set_xlabel("aggregate throughput (Mops/s)")
    ax.set_title("iter-11A — best throughput per YCSB workload  (T=64, 2 hosts)",
                 pad=10)

    # 20 Mops/s target reference line
    ax.axvline(x=TARGET_MOPS, color=STYLE_B_TARGET, linestyle="--",
               linewidth=1.4, label=f"target {TARGET_MOPS:.0f} Mops/s")

    # Bar-end value annotations
    xmax = max(values + [TARGET_MOPS]) * 1.18
    ax.set_xlim(0, xmax)
    for y, v in zip(y_pos, values):
        ax.text(v + xmax * 0.01, y, f"{v:.2f} Mops/s",
                va="center", ha="left", fontsize=11, fontweight="bold",
                color="#222222")

    ax.grid(axis="x", alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc="lower right", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_lat(best, out_path):
    """Horizontal grouped bar chart per workload: write_avg + read_avg µs."""
    fig, ax = plt.subplots(figsize=(8.5, 4.5))
    wls = WORKLOADS
    labels = [wl.replace("workload", "") for wl in wls]
    bar_h = 0.36

    # workload-c has no writes → write_avg == 0; we still plot the read
    # bar at its row but suppress the (empty) write bar.
    w_vals = [best[wl]["w_avg_us"] for wl in wls]
    r_vals = [best[wl]["r_avg_us"] for wl in wls]

    # Two-colour palette from plot_style.py SEABORN_DEEP
    WRITE_C = "#c44e52"  # red — write (focal, since A's write is dominant)
    READ_C  = "#4c72b0"  # blue — read

    y_centre = list(range(len(wls)))[::-1]
    y_write  = [y + bar_h / 2.0 for y in y_centre]
    y_read   = [y - bar_h / 2.0 for y in y_centre]

    bars_w = ax.barh(y_write, w_vals, height=bar_h, color=WRITE_C,
                     edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW,
                     label="write avg")
    bars_r = ax.barh(y_read,  r_vals, height=bar_h, color=READ_C,
                     edgecolor=STYLE_B_EDGE, linewidth=STYLE_B_EDGE_LW,
                     label="read avg")

    ax.set_yticks(y_centre)
    ax.set_yticklabels(labels, fontsize=12)
    ax.set_xlabel("per-op latency (µs)")
    ax.set_title("iter-11A — latency at the best-throughput cell  "
                 "(T=64, 2 hosts)", pad=10)

    xmax = max(w_vals + r_vals) * 1.18
    ax.set_xlim(0, xmax)

    # Bar-end annotations
    for y, v in zip(y_write, w_vals):
        if v > 0:
            ax.text(v + xmax * 0.01, y, f"{v:.1f} µs",
                    va="center", ha="left", fontsize=9.5,
                    fontweight="bold", color=WRITE_C)
        else:
            ax.text(0.1, y, "no writes (workload-c)",
                    va="center", ha="left", fontsize=9, fontstyle="italic",
                    color="#888888")
    for y, v in zip(y_read, r_vals):
        ax.text(v + xmax * 0.01, y, f"{v:.1f} µs",
                va="center", ha="left", fontsize=9.5,
                fontweight="bold", color=READ_C)

    ax.grid(axis="x", alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc="lower right", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    sweep_dir = Path(sys.argv[1] if len(sys.argv) > 1
                     else "docs/g34_scaling_ycsb_iter11A_20260511_042814")
    rows = parse(sweep_dir / "SUMMARY.log")
    if not rows:
        print(f"no parsable rows in {sweep_dir}/SUMMARY.log", file=sys.stderr)
        sys.exit(1)
    best = best_per_workload(rows)

    # Print summary so user can sanity-check
    print("best per workload:")
    for wl in WORKLOADS:
        b = best[wl]
        print(f"  {wl}: {b['agg_mops']:6.2f} Mops/s   "
              f"w_avg={b['w_avg_us']:6.2f}  r_avg={b['r_avg_us']:6.2f}  "
              f"(T={b['T']}, cache={b['cache']}, kv={b['kv']})")

    plot_thpt(best, sweep_dir / "A_best_thpt_per_workload.png")
    plot_lat(best, sweep_dir / "A_best_lat_per_workload.png")


if __name__ == "__main__":
    main()
