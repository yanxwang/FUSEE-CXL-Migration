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
    """Vertical bar chart: x=workload, y=throughput (Mops/s).

    Project Style B: single-class data → uniform mid-grey bars
    (STYLE_B[1]) with project edge colour. Target line in
    STYLE_B_TARGET grey-dashed. Values annotated above each bar.
    """
    fig, ax = plt.subplots(figsize=(9, 5))
    wls = WORKLOADS  # fixed order a,b,c,d,f
    values = [best[wl]["agg_mops"] for wl in wls]
    labels = [wl.replace("workload", "") for wl in wls]

    x_pos = list(range(len(wls)))
    ax.bar(x_pos, values, color=STYLE_B[1])  # uniform mid-grey

    ax.set_xticks(x_pos)
    ax.set_xticklabels(labels, fontsize=12)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("aggregate throughput (Mops/s)")
    ax.set_title(
        "iter-11A — best throughput per YCSB workload  (T=64, 2 hosts)")

    # Project standard polish: edges + headroom + y-grid
    bar_with_headroom(ax, headroom=1.25)

    # 20 Mops/s target reference line (after headroom so it doesn't
    # over-shrink the y limit when below the bars).
    ax.axhline(y=TARGET_MOPS, color=STYLE_B_TARGET, linestyle="--",
               linewidth=1.4, label=f"target {TARGET_MOPS:.0f} Mops/s")
    # Re-apply headroom in case target line sits above bars.
    cur_ymax = ax.get_ylim()[1]
    needed_ymax = max(values + [TARGET_MOPS]) * 1.25
    if needed_ymax > cur_ymax:
        ax.set_ylim(0, needed_ymax)

    # Bar-top value annotations
    ymax = ax.get_ylim()[1]
    for x, v in zip(x_pos, values):
        ax.text(x, v + ymax * 0.012, f"{v:.2f}",
                va="bottom", ha="center", fontsize=11, fontweight="bold",
                color=STYLE_B_EDGE)

    ax.legend(loc="upper right", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_lat(best, out_path):
    """Vertical grouped bar chart: x=workload, y=latency.

    Project Style B: 2-class ordered (read < write in focality) →
    STYLE_B[1] mid-grey for read (less focal), STYLE_B[2] accent
    red for write (focal — Protocol A write path dominates cost).
    """
    fig, ax = plt.subplots(figsize=(9, 5))
    wls = WORKLOADS
    labels = [wl.replace("workload", "") for wl in wls]
    bar_w = 0.36

    w_vals = [best[wl]["w_avg_us"] for wl in wls]
    r_vals = [best[wl]["r_avg_us"] for wl in wls]

    # Project Style B 2-class: focal accent + neutral grey
    WRITE_C = STYLE_B[2]   # #c44e52 accent red (focal)
    READ_C  = STYLE_B[1]   # #969696 mid grey  (less focal)

    x_centre = list(range(len(wls)))
    x_write  = [x - bar_w / 2.0 for x in x_centre]
    x_read   = [x + bar_w / 2.0 for x in x_centre]

    ax.bar(x_write, w_vals, width=bar_w, color=WRITE_C, label="write avg")
    ax.bar(x_read,  r_vals, width=bar_w, color=READ_C,  label="read avg")

    ax.set_xticks(x_centre)
    ax.set_xticklabels(labels, fontsize=12)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("per-op latency (µs)")
    ax.set_title(
        "iter-11A — latency at the best-throughput cell  (T=64, 2 hosts)")

    # Project standard polish
    bar_with_headroom(ax, headroom=1.25)
    ymax = ax.get_ylim()[1]

    # Bar-top annotations
    for x, v in zip(x_write, w_vals):
        if v > 0:
            ax.text(x, v + ymax * 0.012, f"{v:.1f}",
                    va="bottom", ha="center", fontsize=9.5,
                    fontweight="bold", color=STYLE_B_EDGE)
        else:
            ax.text(x, ymax * 0.02, "no\nwrites",
                    va="bottom", ha="center", fontsize=8.5,
                    fontstyle="italic", color=STYLE_B_TARGET)
    for x, v in zip(x_read, r_vals):
        ax.text(x, v + ymax * 0.012, f"{v:.1f}",
                va="bottom", ha="center", fontsize=9.5,
                fontweight="bold", color=STYLE_B_EDGE)

    ax.legend(loc="upper right", fontsize=10)
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
