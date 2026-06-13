#!/usr/bin/env python3
"""Generate summary-table PNG per phase, saved alongside the lin+log plot."""

import csv
import os
import sys
import subprocess
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def latest_dir(prefix):
    out = subprocess.check_output(
        f"ls -dt /home/yanwang/FUSEE/docs/{prefix}*/ 2>/dev/null | head -1",
        shell=True, text=True).strip()
    return out if out else None


def render_table_png(headers, data, out_path, title):
    """Render a matplotlib table to PNG."""
    fig, ax = plt.subplots(figsize=(max(8, 1.5 + 1.5 * len(headers)),
                                     max(2, 0.4 * (len(data) + 2))))
    ax.axis("off")
    table = ax.table(cellText=data, colLabels=headers,
                     loc="center", cellLoc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(11)
    table.scale(1.0, 1.4)
    # Header style
    for i in range(len(headers)):
        table[(0, i)].set_facecolor("#404040")
        table[(0, i)].set_text_props(color="white", weight="bold")
    ax.set_title(title, fontsize=12, pad=16)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[table] wrote {out_path}")


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else 0.0


def load_csv(p):
    with open(p) as f:
        return list(csv.DictReader(f))


def phase1_table(rows, out_path):
    bins = defaultdict(list)
    for r in rows:
        bins[(int(r['V']), r['scenario'])].append(float(r['thpt_Mops']))
    Vs = sorted({v for v, _ in bins.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    data = []
    for V in Vs:
        row = [str(V)]
        for sc in scenarios:
            v = median(bins.get((V, sc), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["V (bytes)"] + scenarios, data, out_path,
                     "Phase 1: V slice — thpt (Mops/s) median of 3 reps\n"
                     "(T=64, cache=10%, zipf θ=0.99, post-fix)")


def phase2_table(rows, out_path):
    bins = defaultdict(list)
    for r in rows:
        bins[(int(r['T']), r['scenario'])].append(float(r['thpt_Mops']))
    Ts = sorted({k[0] for k in bins.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    data = []
    for T in Ts:
        row = [str(T)]
        for sc in scenarios:
            v = median(bins.get((T, sc), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["T"] + scenarios, data, out_path,
                     "Phase 2: T slice — thpt (Mops/s) median of 3 reps\n"
                     "(V=1024, cache=10%, zipf θ=0.99)")


def phase3_table(rows, out_path):
    bins = defaultdict(list)
    for r in rows:
        bins[(int(r['cache_pct']), r['scenario'])].append(float(r['thpt_Mops']))
    pcts = sorted({k[0] for k in bins.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    data = []
    for p in pcts:
        row = [f"{p}%"]
        for sc in scenarios:
            v = median(bins.get((p, sc), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["cache%"] + scenarios, data, out_path,
                     "Phase 3: cache% slice — thpt (Mops/s) median of 3 reps\n"
                     "(V=1024, T=64, zipf θ=0.99)")


def phase4_table(rows, out_path):
    bins = defaultdict(list)
    for r in rows:
        bins[(r['keydist'], r['scenario'])].append(float(r['thpt_Mops']))
    dists = ["uniform", "zipf-0.5", "zipf-0.99", "zipf-1.5"]
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    data = []
    for d in dists:
        row = [d]
        for sc in scenarios:
            v = median(bins.get((d, sc), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["distribution"] + scenarios, data, out_path,
                     "Phase 4: distribution slice — thpt (Mops/s) median of 3 reps\n"
                     "(V=1024, T=64, cache=10%)")


def phase5_table(rows, out_path):
    def xhost_pct(sc):
        if sc == "local_read":  return 0, "READ"
        if sc == "local_write": return 0, "WRITE"
        if sc == "xhost_read":  return 100, "READ"
        if sc == "xhost_write": return 100, "WRITE"
        if sc.startswith("mix_read_"):  return int(sc[len("mix_read_"):]), "READ"
        if sc.startswith("mix_write_"): return int(sc[len("mix_write_"):]), "WRITE"
        return None, None
    bins = defaultdict(list)
    for r in rows:
        p, op = xhost_pct(r['scenario'])
        if p is None:
            continue
        bins[(p, op)].append(float(r['thpt_Mops']))
    pcts = sorted({k[0] for k in bins.keys()})
    data = []
    for p in pcts:
        local_pct = 100 - p
        row = [f"{local_pct}%/{p}%"]
        for op in ["READ", "WRITE"]:
            v = median(bins.get((p, op), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["local%/xhost%", "READ", "WRITE"], data, out_path,
                     "Phase 5: xhost% slice (core + ext) — thpt (Mops/s) median of 3 reps\n"
                     "(V=1024, T=64, cache=10%, zipf θ=0.99)")


def phase6d_table(rows, out_path):
    """Phase 6d uniform: same shape as Phase 3."""
    bins = defaultdict(list)
    for r in rows:
        bins[(int(r['cache_pct']), r['scenario'])].append(float(r['thpt_Mops']))
    pcts = sorted({k[0] for k in bins.keys()})
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    data = []
    for p in pcts:
        row = [f"{p}%"]
        for sc in scenarios:
            v = median(bins.get((p, sc), []))
            row.append(f"{v:.3f}")
        data.append(row)
    render_table_png(["cache%"] + scenarios, data, out_path,
                     "Phase 6d: cache% slice w/ UNIFORM — thpt (Mops/s) median of 3 reps\n"
                     "(V=1024, T=64, uniform distribution)")


def main():
    # Find each phase's latest dir
    phase_dirs = {
        1: latest_dir("iter15A_microbench_phase1_salvaged_"),
        2: latest_dir("iter15A_microbench_phase2_2"),  # avoid catching phase2_xxx_xxx accidentally
        3: latest_dir("iter15A_microbench_phase3_2"),
        4: latest_dir("iter15A_microbench_phase4_2"),
        5: latest_dir("iter15A_microbench_phase5_combined_"),
        "6d": latest_dir("iter15A_microbench_phase6d_uniform_"),
    }
    tablefns = {
        1: phase1_table, 2: phase2_table, 3: phase3_table,
        4: phase4_table, 5: phase5_table,
        "6d": phase6d_table,
    }
    for phase_id, pdir in phase_dirs.items():
        if not pdir:
            print(f"phase{phase_id}: no dir found, skipping")
            continue
        csv_path = os.path.join(pdir, "grid.csv")
        if not os.path.exists(csv_path):
            print(f"phase{phase_id}: no grid.csv at {csv_path}")
            continue
        rows = load_csv(csv_path)
        suffix = str(phase_id) if phase_id != "6d" else "6d_uniform"
        out_path = os.path.join(pdir, f"phase{suffix}_summary_table.png")
        tablefns[phase_id](rows, out_path)


if __name__ == "__main__":
    main()
