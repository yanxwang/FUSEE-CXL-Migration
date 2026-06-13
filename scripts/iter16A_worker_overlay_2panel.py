#!/usr/bin/env python3
"""Re-draw iter-16A worker_stages_overlay with side-by-side log + linear panels.

Usage:
  python3 scripts/iter16A_worker_overlay_2panel.py <aggregate.csv>

Output: worker_stages_overlay_2panel.png in same dir as the CSV.
Left panel = log-log (matches existing worker_stages_overlay.png).
Right panel = linear-linear with the same series.
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: iter16A_worker_overlay_2panel.py <aggregate.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

WORKER_STAGES = ["Stage1", "Stage2", "Stage3", "Stage4", "Stage5", "StageW"]

# Load rows
data = defaultdict(lambda: defaultdict(list))  # data[T][col] = list across reps
with open(CSV) as f:
    rd = csv.DictReader(f)
    for r in rd:
        try:
            T = int(r["T"])
        except (ValueError, KeyError):
            continue
        for k, v in r.items():
            if not k or not v:
                continue
            try:
                data[T][k].append(float(v))
            except ValueError:
                pass

Ts = sorted(data.keys())

def med(values):
    return statistics.median(values) if values else 0.0

# Compute medians per stage per T
medians = {}
for sid in WORKER_STAGES:
    medians[sid] = [med(data[T].get(f"{sid.lower()}_p50_ns", [])) for T in Ts]

colors = plt.cm.tab10.colors

fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(14, 5.5))

# Left panel: log-log
for i, sid in enumerate(WORKER_STAGES):
    ax_log.plot(Ts, medians[sid], marker="o", linewidth=2,
                color=colors[i % 10], label=f"{sid} p50")
ax_log.set_xscale("log", base=2)
ax_log.set_yscale("log")
ax_log.set_xticks(Ts)
ax_log.set_xticklabels([str(t) for t in Ts])
ax_log.set_xlabel("T (workers per host)")
ax_log.set_ylabel("Latency (ns, log)")
ax_log.set_title("Worker stages p50 vs T — log scale", fontsize=11)
ax_log.grid(True, alpha=0.3, which="both")
ax_log.legend(fontsize=9, loc="upper left")

# Right panel: linear-linear
for i, sid in enumerate(WORKER_STAGES):
    ax_lin.plot(Ts, medians[sid], marker="o", linewidth=2,
                color=colors[i % 10], label=f"{sid} p50")
ax_lin.set_xticks(Ts)
ax_lin.set_xticklabels([str(t) for t in Ts])
ax_lin.set_xlabel("T (workers per host)")
ax_lin.set_ylabel("Latency (ns, linear)")
ax_lin.set_title("Worker stages p50 vs T — linear scale", fontsize=11)
ax_lin.grid(True, alpha=0.3)
ax_lin.legend(fontsize=9, loc="upper left")

fig.suptitle("iter-16A xhost_write worker stage decomp (probe-on, V=1024, zipf-0.99)",
             fontsize=12, y=1.01)

out = os.path.join(OUT_DIR, "worker_stages_overlay_2panel.png")
plt.tight_layout()
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")
