#!/usr/bin/env python3
"""iter-16A generic decomp viz — plots per-stage p50/p99 vs an x-axis column,
grouped by another column.

Usage:
  iter16A_decomp_viz_generic.py <aggregate.csv> --x-col <col> --group-col <col> [--label <name>]

Examples:
  # V sweep: x=V, group by T
  iter16A_decomp_viz_generic.py V_sweep_agg.csv --x-col V --group-col T --label V_sweep

  # Dist sweep: x=keydist, group by T
  iter16A_decomp_viz_generic.py dist_sweep_agg.csv --x-col keydist --group-col T --label dist_sweep
"""
import csv, sys, os, statistics, argparse
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

STAGES = [
    ("Stage1", "slot_reserve",  False),
    ("Stage2", "slot_wait",     False),
    ("Stage3", "value_xfer",    True),
    ("Stage4", "ctrl_publish",  True),
    ("Stage5", "ack_wait",      False),
    ("Stage6", "rcv_poll",      False),
    ("Stage7", "rcv_work",      False),
    ("Stage8", "ack_publish",   True),
]
AGGREGATES = [("StageW", "worker total"), ("StageR", "receiver total"), ("RTT", "RTT")]

ap = argparse.ArgumentParser()
ap.add_argument("csv")
ap.add_argument("--x-col", required=True)
ap.add_argument("--group-col", required=True)
ap.add_argument("--label", default="sweep")
args = ap.parse_args()

OUT_DIR = os.path.dirname(os.path.abspath(args.csv))

# Load: data[x_value][group_value][metric] = list across reps
data = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
thpt = defaultdict(lambda: defaultdict(list))
with open(args.csv) as f:
    r = csv.DictReader(f)
    for row in r:
        try:
            x = row[args.x_col]
            g = int(row[args.group_col]) if row[args.group_col].lstrip("-").isdigit() else row[args.group_col]
            try: x = int(x)
            except ValueError: pass
            for k, v in row.items():
                if k in (args.x_col, args.group_col, "rep"): continue
                try: data[x][g][k].append(float(v))
                except (ValueError, TypeError): pass
            try: thpt[x][g].append(float(row["thpt_Mops"]))
            except (ValueError, KeyError): pass
        except KeyError:
            continue

xs = sorted(data.keys(), key=lambda v: (isinstance(v, str), v))
groups = sorted({g for x in data for g in data[x]}, key=lambda v: (isinstance(v, str), v))


def med(vals):
    return statistics.median(vals) if vals else 0


def per_stage_plot(stage_id, stage_label, ovh):
    fig, ax = plt.subplots(figsize=(7.5, 4.8))
    colors = plt.cm.tab10.colors
    for i, g in enumerate(groups):
        p50_vals = [med(data[x][g].get(f"{stage_id.lower()}_p50_ns", [])) for x in xs]
        p99_vals = [med(data[x][g].get(f"{stage_id.lower()}_p99_ns", [])) for x in xs]
        ax.plot(xs, p50_vals, marker="o", linewidth=2, color=colors[i % 10],
                label=f"{args.group_col}={g} p50")
        ax.plot(xs, p99_vals, marker="s", linewidth=1, color=colors[i % 10],
                linestyle="--", alpha=0.6, label=f"{args.group_col}={g} p99")
    ax.set_xlabel(args.x_col)
    ax.set_ylabel("Latency (ns)")
    title = f"{stage_id} = {stage_label} — vs {args.x_col} (grouped by {args.group_col})"
    if ovh: title += "\n[CAVEAT: probe-overhead-bound]"
    ax.set_title(title, fontsize=10)
    ax.grid(True, alpha=0.3); ax.legend(fontsize=8)
    if all(isinstance(x, (int, float)) and x > 0 for x in xs):
        ax.set_xscale("log", base=2 if max(xs)/min(xs) < 256 else 10)
        ax.set_xticks(xs); ax.set_xticklabels([str(x) for x in xs])
    out = os.path.join(OUT_DIR, f"{args.label}_{stage_id}_p50p99.png")
    plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
    print(f"wrote {out}")


for sid, label, ovh in STAGES:
    per_stage_plot(sid, label, ovh)
for sid, label in AGGREGATES:
    per_stage_plot(sid, label, False)

# Throughput plot
fig, ax = plt.subplots(figsize=(7.5, 4.8))
colors = plt.cm.tab10.colors
for i, g in enumerate(groups):
    t_vals = [med(thpt[x].get(g, [])) for x in xs]
    ax.plot(xs, t_vals, marker="o", linewidth=2, color=colors[i % 10],
            label=f"{args.group_col}={g}")
ax.set_xlabel(args.x_col); ax.set_ylabel("Cluster throughput (Mops/s)")
ax.set_title(f"thpt vs {args.x_col} (grouped by {args.group_col}) — probe-on")
ax.grid(True, alpha=0.3); ax.legend()
if all(isinstance(x, (int, float)) and x > 0 for x in xs):
    ax.set_xscale("log", base=2 if max(xs)/min(xs) < 256 else 10)
    ax.set_xticks(xs); ax.set_xticklabels([str(x) for x in xs])
out = os.path.join(OUT_DIR, f"{args.label}_thpt.png")
plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
print(f"wrote {out}")

# Text summary
out = os.path.join(OUT_DIR, f"{args.label}_summary.txt")
with open(out, "w") as f:
    f.write(f"iter-16A {args.label} — median across reps\n")
    f.write("=" * 90 + "\n")
    f.write(f"{args.x_col}\t{args.group_col}\tStageW(ns)\tStageR(ns)\tRTT(ns)\tthpt_Mops\n")
    for x in xs:
        for g in groups:
            sw = med(data[x][g].get("stagew_p50_ns", []))
            sr = med(data[x][g].get("stager_p50_ns", []))
            rt = med(data[x][g].get("rtt_p50_ns", []))
            tp = med(thpt[x].get(g, []))
            f.write(f"{x}\t{g}\t{sw:.0f}\t{sr:.0f}\t{rt:.0f}\t{tp:.3f}\n")
print(f"wrote {out}")
