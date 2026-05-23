#!/usr/bin/env python3
"""iter-18A Phase 4 — 7-group × 2-dist plot suite.

Mirrors iter-17A xhost_write 7-group plots:
  Plot 1: 7-group thpt vs T, zipf + uniform (log+linear panels)
  Plot 2: 7-group median table (zipf, uniform)
  Plot 3: uniform vs zipf side-by-side + ratio heatmap

Usage:
  iter18A_phase4_plot.py <sweep_dir>
"""
import csv, os, statistics, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SWEEP = sys.argv[1] if len(sys.argv) > 1 else None
if not SWEEP:
    docs = "/home/yanwang/FUSEE/docs"
    cands = sorted(d for d in os.listdir(docs) if d.startswith("iter18A_phase4_7group_sweep_"))
    SWEEP = os.path.join(docs, cands[-1])
CSV = os.path.join(SWEEP, "grid.csv")
print(f"sweep: {SWEEP}")

# Group structure (7 groups, mirroring iter-17A uniform_7group plot)
groups_spec = [
    ("1. path opt N=0 (P3 end)", "worker_id", 0, "#888888", "o"),
    ("2. Plan A worker_id N=0", "worker_id", 0, "#1f77b4", "s"),
    ("3. Plan A worker_id N=4", "worker_id", 4, "#1f77b4", "^"),
    ("4. Plan A worker_id N=8", "worker_id", 8, "#1f77b4", "v"),
    ("5. Plan B key_hash N=0", "key_hash", 0, "#d62728", "x"),
    ("6. Plan B key_hash N=4", "key_hash", 4, "#d62728", "^"),
    ("7. Plan B key_hash N=8", "key_hash", 8, "#d62728", "v"),
]

# Load + medians
data = defaultdict(lambda: defaultdict(list))  # data[(dist, routing, N)][T] = [thpt]
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"]); N = int(r["N"]); routing = r["routing"]; dist = r["dist"]
        thpt = float(r["thpt"]) / 1e6
        if thpt > 0:
            data[(dist, routing, N)][T].append(thpt)
med = {k: {T: statistics.median(v) for T, v in by_T.items() if v} for k, by_T in data.items()}

dists = sorted(set(k[0] for k in med))
Ts = sorted({T for v in med.values() for T in v})

# --- Plot 1: 7-group thpt vs T (log + linear) per dist ---
for dist in dists:
    fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(18, 7))
    for ax, yscale_log in ((ax_log, True), (ax_lin, False)):
        for label, routing, N, color, marker in groups_spec:
            ys = [med.get((dist, routing, N), {}).get(T, float("nan")) for T in Ts]
            ax.plot(Ts, ys, color=color, marker=marker, linewidth=2, markersize=9,
                    label=label, alpha=0.9 if "N=0" in label and "path opt" not in label else 1.0)
        ax.set_xscale("log", base=2)
        if yscale_log:
            ax.set_yscale("log")
            ax.set_ylabel("Cluster throughput (Mops/s, log)", fontsize=12)
            ax.set_title("log-Y", fontsize=12)
        else:
            ax.set_ylabel("Cluster throughput (Mops/s, linear)", fontsize=12)
            ax.set_title("linear-Y", fontsize=12)
        ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
        ax.set_xlabel("T (workers per host)", fontsize=12)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(loc="best", fontsize=9, ncol=2)
    fig.suptitle(f"iter-18A Phase 4 — xhost_read 7-group scaling, dist={dist} V=1024 cache=0 (3-rep median)",
                 fontsize=13, y=0.99)
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(SWEEP, f"7group_thpt_vs_T_{dist}.png")
    plt.savefig(out, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {out}")

# --- Plot 2: 7-group median table per dist ---
for dist in dists:
    fig, ax = plt.subplots(figsize=(14, 0.5 * (len(groups_spec) + 2) + 1.5))
    ax.axis("off")
    headers = ["Group"] + [f"T={T}" for T in Ts]
    cell_text = []
    for label, routing, N, _, _ in groups_spec:
        row = [label]
        for T in Ts:
            v = med.get((dist, routing, N), {}).get(T)
            row.append(f"{v:.3f}" if v is not None else "—")
        cell_text.append(row)
    tbl = ax.table(cellText=cell_text, colLabels=headers, loc="center",
                   cellLoc="right", colLoc="center")
    tbl.auto_set_font_size(False); tbl.set_fontsize(11); tbl.scale(1.0, 1.7)
    for j in range(len(headers)):
        tbl[(0, j)].set_facecolor("#cfcfcf")
        tbl[(0, j)].set_text_props(weight="bold")
    for i in range(1, len(cell_text) + 1):
        tbl[(i, 0)].set_text_props(weight="bold", ha="left")
        tbl[(i, 0)].set_facecolor("#f0f0f0")
    col_widths = [0.32] + [0.097] * len(Ts)
    for i in range(len(cell_text) + 1):
        for j, w in enumerate(col_widths):
            tbl[(i, j)].set_width(w)
    ax.set_title(f"iter-18A Phase 4 — 7-group cluster Mops/s (3-rep median), dist={dist} V=1024 cache=0",
                 fontsize=12, pad=12, weight="bold")
    plt.tight_layout()
    out = os.path.join(SWEEP, f"7group_table_{dist}.png")
    plt.savefig(out, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {out}")

# --- Plot 3: uniform vs zipf side-by-side ---
if len(dists) >= 2:
    fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(18, 7))
    for ax, yscale_log in ((ax_log, True), (ax_lin, False)):
        for label, routing, N, color, marker in groups_spec:
            if "path opt" in label: continue  # skip Group 1 (≡ Group 2)
            zys = [med.get(("zipf-0.99", routing, N), {}).get(T, float("nan")) for T in Ts]
            uys = [med.get(("uniform", routing, N), {}).get(T, float("nan")) for T in Ts]
            ax.plot(Ts, zys, color=color, marker=marker, linewidth=2, markersize=8,
                    linestyle="-", label=f"{label} zipf")
            ax.plot(Ts, uys, color=color, marker=marker, linewidth=2, markersize=8,
                    linestyle=":", label=f"{label} uniform")
        ax.set_xscale("log", base=2)
        if yscale_log:
            ax.set_yscale("log"); ax.set_title("log-Y", fontsize=12)
        else:
            ax.set_title("linear-Y", fontsize=12)
        ax.set_ylabel("Cluster throughput (Mops/s)", fontsize=12)
        ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
        ax.set_xlabel("T (workers per host)", fontsize=12)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(loc="best", fontsize=8, ncol=2)
    fig.suptitle("iter-18A: uniform (dotted) vs zipf-0.99 (solid) — xhost_read V=1024 cache=0 (3-rep median)",
                 fontsize=13, y=0.99)
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(SWEEP, "uniform_vs_zipf_compare.png")
    plt.savefig(out, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {out}")

    # ratio heatmap
    rows_meta = [(routing, N, lbl) for lbl, routing, N, _, _ in groups_spec if "path opt" not in lbl]
    mat = np.zeros((len(rows_meta), len(Ts)))
    for i, (routing, N, _) in enumerate(rows_meta):
        for j, T in enumerate(Ts):
            z = med.get(("zipf-0.99", routing, N), {}).get(T)
            u = med.get(("uniform", routing, N), {}).get(T)
            mat[i, j] = (u / z) if (z and z > 0 and u is not None) else float("nan")
    fig, ax = plt.subplots(figsize=(13, 4))
    im = ax.imshow(mat, aspect="auto", cmap="RdYlGn", vmin=0.5, vmax=1.5)
    ax.set_xticks(range(len(Ts))); ax.set_xticklabels([f"T={T}" for T in Ts])
    ax.set_yticks(range(len(rows_meta))); ax.set_yticklabels([m[2] for m in rows_meta])
    ax.set_title("uniform / zipf-0.99 throughput ratio — >1 = uniform wins, <1 = zipf wins\n"
                 "(iter-18A read path: zipf 通常领先 due to hot-key cacheline reuse)",
                 fontsize=11.5, pad=10)
    for i in range(len(rows_meta)):
        for j in range(len(Ts)):
            v = mat[i, j]
            if not np.isnan(v):
                color = "black" if 0.7 < v < 1.3 else "white"
                ax.text(j, i, f"{v:.2f}×", ha="center", va="center",
                        color=color, fontsize=9.5, fontweight="bold")
    plt.colorbar(im, ax=ax, label="uniform / zipf ratio")
    plt.tight_layout()
    out = os.path.join(SWEEP, "uniform_vs_zipf_ratio_heatmap.png")
    plt.savefig(out, dpi=140, bbox_inches="tight")
    plt.close()
    print(f"wrote {out}")

# stdout summary
print()
for dist in dists:
    print(f"\n--- {dist} medians (cluster Mops/s) ---")
    print(f"{'Group':<28s} " + " ".join(f"T={T:>3}" for T in Ts))
    for label, routing, N, _, _ in groups_spec:
        row = f"{label:<28s} "
        for T in Ts:
            v = med.get((dist, routing, N), {}).get(T)
            row += f"{v:>5.3f}  " if v is not None else "  NA   "
        print(row)
