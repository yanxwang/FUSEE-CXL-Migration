#!/usr/bin/env python3
"""iter-4 C-only KV=1024 — best-throughput-per-workload summary plots.

Sister script to plot_iter11A_best_per_workload.py, but reads the
iter-4 variable-KV C-only sweep at FUSEE_VALUE_SIZE=1024
(docs/g34_scaling_ycsb_C_only_20260424_191839/SUMMARY.log).

The iter-4 sweep line format differs from iter-11A:
  - has `value_size=N` field between cache= and num_hosts=
  - `trans_wall_max=N.NNNs` (trailing 's' suffix)
  - `# wl_optC_t<T>_cache<on|off>` (no `_rep<R>_kv<N>` suffix)

Plots produced:
  - C_best_thpt_per_workload.png  (Title-cased)
  - C_best_lat_per_workload.png

Run:
  python3 scripts/plot_iter4_C_best_per_workload.py [sweep_dir]
"""

import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "docs", "tools"))
from plot_style import (  # noqa: E402
    apply_style, STYLE_B, STYLE_B_EDGE, STYLE_B_EDGE_LW, STYLE_B_TARGET,
    bar_with_headroom,
)

apply_style()

# iter-4 C-only sweep line format
LINE = re.compile(
    r"YCSB opt=([ABC]) cache=(\d+) value_size=(\d+) num_hosts=\d+ "
    r"threads=(\d+) threads_eff=\d+ "
    r"load_ops=\d+ load_thpt=\d+ "
    r"trans_ops=\d+ trans_wall_max=[\d\.]+s? trans_agg_thpt=(\d+) "
    r"w_avg_ns=(\d+) w_p50_ns=(\d+) w_p99_ns=(\d+) "
    r"r_avg_ns=(\d+) r_p50_ns=(\d+) r_p99_ns=(\d+)\s+"
    r"# (\w+)_opt([ABC])_t\d+_cache(on|off)"
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
            (opt, cache_int, vs, T, agg,
             w_avg, w_p50, w_p99,
             r_avg, r_p50, r_p99,
             wl, opt2, cache_str) = m.groups()
            rows.append({
                "opt": opt, "wl": wl, "T": int(T),
                "cache": cache_str, "kv": int(vs),
                "agg_mops": int(agg) / 1e6,
                "w_avg_us": int(w_avg) / 1000.0,
                "w_p99_us": int(w_p99) / 1000.0,
                "r_avg_us": int(r_avg) / 1000.0,
                "r_p99_us": int(r_p99) / 1000.0,
            })
    return rows


def best_per_workload_C(rows):
    by_wl = {}
    for r in rows:
        if r["opt"] != "C":
            continue
        by_wl.setdefault(r["wl"], []).append(r)
    out = {}
    for wl, runs in by_wl.items():
        out[wl] = max(runs, key=lambda r: r["agg_mops"])
    return out


def plot_thpt(best, out_path):
    fig, ax = plt.subplots(figsize=(9, 5))
    wls = WORKLOADS
    values = [best[wl]["agg_mops"] for wl in wls]
    labels = [wl.replace("workload", "") for wl in wls]

    palette = [STYLE_B[2], STYLE_B[1], "#FFFFFF"]
    colours = [palette[i % len(palette)] for i in range(len(wls))]

    x_pos = list(range(len(wls)))
    ax.bar(x_pos, values, color=colours, width=0.4)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(labels, fontsize=12)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("aggregate throughput (Mops/s)")
    ax.set_title(
        "Throughput per YCSB Workload (1024B, T=64, 2 Hosts) — Protocol C")

    bar_with_headroom(ax, headroom=1.25)

    ax.axhline(y=TARGET_MOPS, color=STYLE_B_TARGET, linestyle="--",
               linewidth=1.4, label=f"target {TARGET_MOPS:.0f} Mops/s")
    cur_ymax = ax.get_ylim()[1]
    needed_ymax = max(values + [TARGET_MOPS]) * 1.25
    if needed_ymax > cur_ymax:
        ax.set_ylim(0, needed_ymax)

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
    fig, ax = plt.subplots(figsize=(9, 5))
    wls = WORKLOADS
    labels = [wl.replace("workload", "") for wl in wls]
    bar_w = 0.36

    w_vals = [best[wl]["w_avg_us"] for wl in wls]
    r_vals = [best[wl]["r_avg_us"] for wl in wls]

    WRITE_C = STYLE_B[2]   # accent red
    READ_C  = STYLE_B[1]   # mid grey

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
        "Latency per YCSB Workload (1024B, T=64, 2 Hosts) — Protocol C")

    bar_with_headroom(ax, headroom=1.25)
    ymax = ax.get_ylim()[1]

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
                     else "docs/g34_scaling_ycsb_C_only_20260424_191839")
    rows = parse(sweep_dir / "SUMMARY.log")
    if not rows:
        print(f"no parsable rows in {sweep_dir}/SUMMARY.log", file=sys.stderr)
        sys.exit(1)
    best = best_per_workload_C(rows)

    print("best per workload (C, KV=1024):")
    for wl in WORKLOADS:
        b = best[wl]
        print(f"  {wl}: {b['agg_mops']:6.2f} Mops/s   "
              f"w_avg={b['w_avg_us']:6.2f}  r_avg={b['r_avg_us']:6.2f}  "
              f"(T={b['T']}, cache={b['cache']})")

    plot_thpt(best, sweep_dir / "C_best_thpt_per_workload.png")
    plot_lat(best, sweep_dir / "C_best_lat_per_workload.png")


if __name__ == "__main__":
    main()
