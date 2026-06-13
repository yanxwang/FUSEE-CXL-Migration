#!/usr/bin/env python3
"""3 plots requested:
  1. uniform 7-group thpt vs T (line plot, log+linear) — in 8group_uniform dir
  2. uniform 7-group thpt table (matplotlib table image) — in 8group_uniform dir
  3. zipf-0.99 8-group thpt table (matplotlib table image) — in 8group_final dir
"""
import csv, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

UNIFORM_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_8group_uniform_20260523_000353"
ZIPF_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_8group_final"

# ----- Collect medians from sweep dirs -----
def collect(sweep_dirs):
    data = defaultdict(lambda: defaultdict(list))
    for d in sweep_dirs:
        csv_path = os.path.join(d, "grid.csv")
        if not os.path.exists(csv_path):
            continue
        with open(csv_path) as f:
            for r in csv.DictReader(f):
                try:
                    T = int(r["T"])
                    N = int(r["N"])
                    routing = r["routing"]
                    thpt_col = "thpt" if "thpt" in r else ("thpt_cluster_ops" if "thpt_cluster_ops" in r else "thpt_h0_ops")
                    thpt = float(r[thpt_col]) / 1e6
                    if thpt > 0:
                        data[(N, routing)][T].append(thpt)
                except (KeyError, ValueError):
                    continue
    return {k: {T: statistics.median(v) for T, v in by_T.items() if v} for k, by_T in data.items()}

SWEEP_ZIPF = [
    "/home/yanwang/FUSEE/docs/iter17A_scaling_finals_20260522_084104",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_fill_20260522_081401",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_20260522_075307",
]
SWEEP_UNIFORM = [UNIFORM_DIR]

zipf = collect(SWEEP_ZIPF)
uniform = collect(SWEEP_UNIFORM)

# iter-15A baseline (zipf-0.99 only)
def iter15A_zipf_baseline():
    csv_path = "/home/yanwang/FUSEE/docs/iter15A_microbench_phase2_20260520_063314/grid.csv"
    by_T = defaultdict(list)
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            if r["scenario"] != "xhost_write": continue
            if r["keydist"] != "zipf-0.99": continue
            if r["V"] != "1024": continue
            T = int(r["T"])
            by_T[T].append(float(r["thpt_Mops"]))
    return {T: statistics.median(v) for T, v in by_T.items()}

iter15A = iter15A_zipf_baseline()
Ts = [1, 2, 4, 8, 16, 32, 64]

# ----- Group definitions -----
def zipf_groups():
    return [
        ("1. iter-15A baseline (single ring+recv)", iter15A,                       "#888888", "o", "-"),
        ("2. iter-17A path opt N=0",                zipf.get((0, "worker_id"), {}), "#1f77b4", "s", "-"),
        ("3. Plan A worker_id N=0",                 zipf.get((0, "worker_id"), {}), "#1f77b4", "x", "--"),
        ("4. Plan A worker_id N=4",                 zipf.get((4, "worker_id"), {}), "#d62728", "^", "-"),
        ("5. Plan A worker_id N=8",                 zipf.get((8, "worker_id"), {}), "#d62728", "v", "--"),
        ("6. Plan B key_hash  N=0",                 zipf.get((0, "key_hash"), {}),  "#2ca02c", "x", "--"),
        ("7. Plan B key_hash  N=4",                 zipf.get((4, "key_hash"), {}),  "#2ca02c", "^", "-"),
        ("8. Plan B key_hash  N=8",                 zipf.get((8, "key_hash"), {}),  "#9467bd", "v", "--"),
    ]

def uniform_groups():
    return [
        # group 1 (iter-15A baseline uniform) is N/A — phase2 only ran zipf
        ("2. iter-17A path opt N=0",                uniform.get((0, "worker_id"), {}), "#1f77b4", "s", "-"),
        ("3. Plan A worker_id N=0",                 uniform.get((0, "worker_id"), {}), "#1f77b4", "x", "--"),
        ("4. Plan A worker_id N=4",                 uniform.get((4, "worker_id"), {}), "#d62728", "^", "-"),
        ("5. Plan A worker_id N=8",                 uniform.get((8, "worker_id"), {}), "#d62728", "v", "--"),
        ("6. Plan B key_hash  N=0",                 uniform.get((0, "key_hash"), {}),  "#2ca02c", "x", "--"),
        ("7. Plan B key_hash  N=4",                 uniform.get((4, "key_hash"), {}),  "#2ca02c", "^", "-"),
        ("8. Plan B key_hash  N=8",                 uniform.get((8, "key_hash"), {}),  "#9467bd", "v", "--"),
    ]

# ===== Plot 1: uniform 7-group line plot (log + linear) =====
def line_plot(groups, title, outfile):
    fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(20, 8))
    def draw_panel(ax, yscale_log):
        for label, d, color, marker, ls in groups:
            ys = [d.get(T, float("nan")) for T in Ts]
            ax.plot(Ts, ys, color=color, marker=marker, linestyle=ls,
                    linewidth=2, markersize=9, label=label)
        ax.axhline(y=20, color="#aa00aa", linestyle="--", alpha=0.7, linewidth=2)
        target_y = 21 if yscale_log else 19
        ax.text(1.05, target_y, "20 Mops/s cluster target", fontsize=10,
                color="#aa00aa", fontweight="bold",
                va="bottom" if yscale_log else "top")
        ax.set_xscale("log", base=2)
        if yscale_log:
            ax.set_yscale("log")
            ax.set_ylabel("Cluster throughput (Mops/s, log scale)", fontsize=12)
            ax.set_title("log-Y", fontsize=12)
        else:
            ax.set_ylim(0, 22)
            ax.set_ylabel("Cluster throughput (Mops/s, linear scale)", fontsize=12)
            ax.set_title("linear-Y", fontsize=12)
        ax.set_xticks(Ts)
        ax.set_xticklabels([str(t) for t in Ts])
        ax.set_xlabel("T (workers per host)", fontsize=12)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(loc="upper left", fontsize=9, ncol=2)
    draw_panel(ax_log, True)
    draw_panel(ax_lin, False)
    fig.suptitle(title, fontsize=14, y=0.99)
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.savefig(outfile, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {outfile}")

line_plot(
    uniform_groups(),
    "iter-17A 7-group scaling — uniform xhost_write V=1024 (cluster Mops/s, 3-rep median)",
    os.path.join(UNIFORM_DIR, "uniform_7group_thpt_vs_T.png")
)

# ===== Plot 2: uniform 7-group table image =====
def table_plot(groups, title, outfile):
    n_rows = len(groups)
    fig, ax = plt.subplots(figsize=(14, 0.5 * (n_rows + 2) + 1.5))
    ax.axis("off")
    # Build rows
    headers = ["Group"] + [f"T={T}" for T in Ts]
    cell_text = []
    for label, d, *_ in groups:
        row = [label]
        for T in Ts:
            v = d.get(T)
            row.append(f"{v:.3f}" if v is not None else "—")
        cell_text.append(row)

    tbl = ax.table(cellText=cell_text, colLabels=headers, loc="center",
                   cellLoc="right", colLoc="center")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(11)
    tbl.scale(1.0, 1.7)
    # Style: header row + first col bold; gray bg for header
    for j in range(len(headers)):
        tbl[(0, j)].set_facecolor("#dddddd")
        tbl[(0, j)].set_text_props(weight="bold")
    for i in range(1, n_rows + 1):
        tbl[(i, 0)].set_text_props(weight="bold", ha="left")
        tbl[(i, 0)].set_facecolor("#f0f0f0")
    # column widths
    col_widths = [0.32] + [0.097] * len(Ts)
    for i in range(n_rows + 1):
        for j, w in enumerate(col_widths):
            tbl[(i, j)].set_width(w)

    ax.set_title(title, fontsize=13, pad=14, weight="bold")
    plt.tight_layout()
    plt.savefig(outfile, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {outfile}")

table_plot(
    uniform_groups(),
    "iter-17A 7-group cluster throughput (Mops/s, 3-rep median) — xhost_write uniform V=1024",
    os.path.join(UNIFORM_DIR, "uniform_7group_thpt_table.png")
)

# ===== Plot 3: zipf-0.99 8-group table image =====
table_plot(
    zipf_groups(),
    "iter-17A 8-group cluster throughput (Mops/s, 3-rep median) — xhost_write zipf-0.99 V=1024",
    os.path.join(ZIPF_DIR, "8group_thpt_table.png")
)
