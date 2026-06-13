#!/usr/bin/env python3
"""iter-15A microbench plot generator.

Reads a phase's grid.csv and emits 1 figure with linear + log Y subplots
side-by-side, showing all that phase's cells.

Usage:
  python3 iter15A_microbench_plot.py <phase_outdir>
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_csv(path):
    rows = []
    with open(path) as f:
        rd = csv.DictReader(f)
        for r in rd:
            rows.append(r)
    return rows


def median_thpt_per_cell(rows, key_func):
    """Group rows by key_func, median the thpt_Mops across reps."""
    bins = defaultdict(list)
    for r in rows:
        k = key_func(r)
        try:
            bins[k].append(float(r["thpt_Mops"]))
        except (ValueError, KeyError):
            pass
    out = {}
    for k, vs in bins.items():
        if vs:
            vs.sort()
            out[k] = vs[len(vs) // 2]  # median
    return out


def render_lin_log(fig_path, xs, x_log_base, x_label, lines, title):
    """lines = list of (label, ys). Render side-by-side linear + log y."""
    fig, (ax_lin, ax_log) = plt.subplots(1, 2, figsize=(15, 6))
    for ax, yscale, sfx in [(ax_lin, "linear", "(linear y)"),
                              (ax_log, "log", "(log y)")]:
        for label, ys in lines:
            ax.plot(xs, ys, marker="o", linewidth=2, label=label)
        if x_log_base:
            ax.set_xscale("log", base=x_log_base)
        ax.set_xticks(xs)
        ax.set_xticklabels([str(x) for x in xs])
        if yscale == "log":
            ax.set_yscale("log")
        ax.set_xlabel(x_label)
        ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.set_title(f"{title} {sfx}")
        ax.legend()
        ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_path, dpi=120)
    plt.close(fig)
    print(f"[plot] wrote {fig_path}")


def render_lin_log_bar(fig_path, xs_categorical, group_labels, group_data, x_label, title):
    """xs_categorical = list of category names.
    group_labels = list of group names (legend entries).
    group_data = list of lists [ys_for_each_x]."""
    fig, (ax_lin, ax_log) = plt.subplots(1, 2, figsize=(15, 6))
    n = len(group_labels)
    width = 0.8 / n
    x_pos = list(range(len(xs_categorical)))
    for ax, yscale, sfx in [(ax_lin, "linear", "(linear y)"),
                              (ax_log, "log", "(log y)")]:
        for i, label in enumerate(group_labels):
            offsets = [xp + (i - (n - 1) / 2) * width for xp in x_pos]
            ax.bar(offsets, group_data[i], width=width, label=label)
        ax.set_xticks(x_pos)
        ax.set_xticklabels(xs_categorical)
        if yscale == "log":
            ax.set_yscale("log")
        ax.set_xlabel(x_label)
        ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.set_title(f"{title} {sfx}")
        ax.legend()
        ax.grid(True, axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_path, dpi=120)
    plt.close(fig)
    print(f"[plot] wrote {fig_path}")


def plot_phase1(rows, out_path):
    cell = median_thpt_per_cell(rows, lambda r: (int(r["V"]), r["scenario"]))
    Vs = sorted({V for V, _ in cell.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    lines = [(sc, [cell.get((V, sc), 0.0) for V in Vs]) for sc in scenarios]
    render_lin_log(out_path, Vs, 2, "Value size V (bytes)", lines,
                   "Phase 1: V slice (T=64, cache=10%, zipf θ=0.99)")


def plot_phase2(rows, out_path):
    cell = median_thpt_per_cell(rows, lambda r: (int(r["T"]), r["scenario"]))
    Ts = sorted({T for T, _ in cell.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    lines = [(sc, [cell.get((T, sc), 0.0) for T in Ts]) for sc in scenarios]
    render_lin_log(out_path, Ts, 2, "Client threads T", lines,
                   "Phase 2: T slice (V=1024, cache=10%, zipf θ=0.99)")


def plot_phase3(rows, out_path):
    cell = median_thpt_per_cell(rows, lambda r: (int(r["cache_pct"]), r["scenario"]))
    pcts = sorted({p for p, _ in cell.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    lines = [(sc, [cell.get((p, sc), 0.0) for p in pcts]) for sc in scenarios]
    render_lin_log(out_path, pcts, 10, "Cache capacity (% of unique keys)", lines,
                   "Phase 3: cache% slice (V=1024, T=64, zipf θ=0.99)")


def plot_phase4(rows, out_path):
    """Phase 4 distribution: x=categorical dist labels, lines per scenario."""
    cell = median_thpt_per_cell(rows, lambda r: (r["keydist"], r["scenario"]))
    dists = ["uniform", "zipf-0.5", "zipf-0.99", "zipf-1.5"]
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    x_pos = list(range(len(dists)))
    fig, (ax_lin, ax_log) = plt.subplots(1, 2, figsize=(15, 6))
    for ax, yscale, sfx in [(ax_lin, "linear", "(linear y)"),
                              (ax_log, "log", "(log y)")]:
        for sc in scenarios:
            ys = [cell.get((d, sc), 0.0) for d in dists]
            ax.plot(x_pos, ys, marker="o", linewidth=2, label=sc)
        ax.set_xticks(x_pos)
        ax.set_xticklabels(dists)
        if yscale == "log":
            ax.set_yscale("log")
        ax.set_xlabel("Key distribution")
        ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.set_title(f"Phase 4: distribution slice {sfx}\n(V=1024, T=64, cache=10%)")
        ax.legend()
        ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_phase5(rows, out_path):
    """Phase 5 xhost% slice: x=xhost%, lines={READ, WRITE}, y=thpt."""
    def xhost_pct_of(sc):
        if sc == "local_read":  return (0, "READ")
        if sc == "local_write": return (0, "WRITE")
        if sc == "xhost_read":  return (100, "READ")
        if sc == "xhost_write": return (100, "WRITE")
        if sc.startswith("mix_read_"):  return (int(sc[len("mix_read_"):]), "READ")
        if sc.startswith("mix_write_"): return (int(sc[len("mix_write_"):]), "WRITE")
        return (None, None)
    cell_op = defaultdict(list)
    for r in rows:
        pct, op = xhost_pct_of(r["scenario"])
        if pct is None:
            continue
        try:
            cell_op[(pct, op)].append(float(r["thpt_Mops"]))
        except ValueError:
            pass
    medians = {k: sorted(v)[len(v)//2] for k, v in cell_op.items() if v}
    pcts = sorted({p for p, _ in medians.keys()})
    lines = [(op, [medians.get((p, op), 0.0) for p in pcts]) for op in ["READ", "WRITE"]]
    render_lin_log(out_path, pcts, None, "Cross-host fraction (%)", lines,
                   "Phase 5: xhost% slice (V=1024, T=64, cache=10%, zipf θ=0.99)")


def plot_phase6d_uniform(rows, out_path):
    """Phase 6d: cache% slice with uniform — same dims as Phase 3."""
    cell = median_thpt_per_cell(rows, lambda r: (int(r["cache_pct"]), r["scenario"]))
    pcts = sorted({p for p, _ in cell.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    lines = [(sc, [cell.get((p, sc), 0.0) for p in pcts]) for sc in scenarios]
    render_lin_log(out_path, pcts, 10, "Cache capacity (% of unique keys)", lines,
                   "Phase 6d: cache% slice w/ UNIFORM dist (V=1024, T=64)")


PHASE_PLOTTERS = {
    "phase1": plot_phase1,
    "phase2": plot_phase2,
    "phase3": plot_phase3,
    "phase4": plot_phase4,
    "phase5": plot_phase5,
    "phase5_ext": plot_phase5,
    "phase6d_uniform": plot_phase6d_uniform,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    args = ap.parse_args()

    csv_path = os.path.join(args.outdir, "grid.csv")
    if not os.path.exists(csv_path):
        print(f"[plot] no grid.csv at {csv_path}", file=sys.stderr)
        return 1
    rows = load_csv(csv_path)
    if not rows:
        print(f"[plot] grid.csv is empty", file=sys.stderr)
        return 1

    phase = rows[0].get("phase", "")
    plotter = PHASE_PLOTTERS.get(phase)
    if plotter is None:
        print(f"[plot] no plotter for phase={phase}", file=sys.stderr)
        return 1
    out_path = os.path.join(args.outdir, f"{phase}_plot_loglin.png")
    plotter(rows, out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
