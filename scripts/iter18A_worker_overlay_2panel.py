#!/usr/bin/env python3
"""iter-18A xhost_read worker stage overlay (log + linear side-by-side).

iter-18A Phase 2.1 T-sweep stored per-T per-op CSV files (decomp_T<N>.csv)
instead of one aggregate CSV. This script reads each, computes p50 medians
per stage per T, and draws the dual-panel overlay matching iter-16A.

Usage:
  python3 scripts/iter18A_worker_overlay_2panel.py <T_sweep_dir>

Output: worker_stages_overlay_2panel.png in the input dir.
"""
import csv, sys, os, statistics, glob, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: iter18A_worker_overlay_2panel.py <T_sweep_dir>")
    sys.exit(1)
SWEEP_DIR = sys.argv[1]

WORKER_STAGES = ["Stage1", "Stage2", "Stage3", "Stage4", "Stage5", "Stage6", "StageW"]

# Collect T → {stage → median} from decomp_T<N>.csv files
files = sorted(glob.glob(os.path.join(SWEEP_DIR, "decomp_T*.csv")))
data = {}  # data[T][stage] = median over ops
for path in files:
    m = re.search(r"decomp_T(\d+)\.csv$", path)
    if not m:
        continue
    T = int(m.group(1))
    per_stage_vals = {s: [] for s in WORKER_STAGES}
    with open(path) as f:
        rd = csv.DictReader(f)
        for row in rd:
            for s in WORKER_STAGES:
                if s in row and row[s]:
                    try:
                        per_stage_vals[s].append(float(row[s]))
                    except ValueError:
                        pass
    data[T] = {s: statistics.median(v) if v else 0.0
               for s, v in per_stage_vals.items()}

Ts = sorted(data.keys())
print(f"# T grid: {Ts}", file=sys.stderr)
print(f"# stages: {WORKER_STAGES}", file=sys.stderr)

medians = {s: [data[T][s] for T in Ts] for s in WORKER_STAGES}

colors = plt.cm.tab10.colors

fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(14, 5.5))

# Left: log-log
for i, sid in enumerate(WORKER_STAGES):
    ax_log.plot(Ts, medians[sid], marker="o", linewidth=2,
                color=colors[i % 10], label=f"{sid} p50")
ax_log.set_xscale("log", base=2)
ax_log.set_yscale("log")
ax_log.set_xticks(Ts)
ax_log.set_xticklabels([str(t) for t in Ts])
ax_log.set_xlabel("T (workers per host)")
ax_log.set_ylabel("Latency (ns, log)")
ax_log.set_title("XR worker stages p50 vs T — log scale", fontsize=11)
ax_log.grid(True, alpha=0.3, which="both")
ax_log.legend(fontsize=9, loc="upper left")

# Right: linear
for i, sid in enumerate(WORKER_STAGES):
    ax_lin.plot(Ts, medians[sid], marker="o", linewidth=2,
                color=colors[i % 10], label=f"{sid} p50")
ax_lin.set_xticks(Ts)
ax_lin.set_xticklabels([str(t) for t in Ts])
ax_lin.set_xlabel("T (workers per host)")
ax_lin.set_ylabel("Latency (ns, linear)")
ax_lin.set_title("XR worker stages p50 vs T — linear scale", fontsize=11)
ax_lin.grid(True, alpha=0.3)
ax_lin.legend(fontsize=9, loc="upper left")

fig.suptitle("iter-18A xhost_read worker stage decomp (probe-on, V=1024, zipf-0.99, N=0)",
             fontsize=12, y=1.01)

out = os.path.join(SWEEP_DIR, "worker_stages_overlay_2panel.png")
plt.tight_layout()
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")
