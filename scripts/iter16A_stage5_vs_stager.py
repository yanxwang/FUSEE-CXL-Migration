#!/usr/bin/env python3
"""Stage 5 (worker ack_wait) vs StageR (= Stage6+7+8, receiver work) comparison.

Shows how the worker-side ack-wait latency is composed:
  - StageR = receiver actually doing work (1.8-3.7 µs across all T)
  - Stage5 - StageR = worker spin overhead + CXL propagation + receiver queueing

At low T, receiver work dominates Stage 5.
At high T, queueing dominates (receiver is throat).
"""
import csv, sys, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = sys.argv[1]
import os
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

by_T = defaultdict(lambda: defaultdict(list))
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"])
        for k, v in r.items():
            if k in ("T","rep"): continue
            try: by_T[T][k].append(float(v))
            except: pass

Ts = sorted(by_T.keys())
med = lambda T, k: statistics.median(by_T[T].get(k, [0]))

stage5 = [med(T, "stage5_p50_ns") for T in Ts]
stage6 = [med(T, "stage6_p50_ns") for T in Ts]
stage7 = [med(T, "stage7_p50_ns") for T in Ts]
stage8 = [med(T, "stage8_p50_ns") for T in Ts]
stager = [med(T, "stager_p50_ns") for T in Ts]
queueing = [s5 - sr for s5, sr in zip(stage5, stager)]
sr_pct = [sr/s5*100 for sr, s5 in zip(stager, stage5)]

# Two-panel: top = stacked composition of Stage 5; bottom = StageR/Stage5 ratio
fig, axes = plt.subplots(2, 1, figsize=(9, 8))

# Panel 1: Stage 5 stacked into [receiver work, queueing]
ax = axes[0]
xs = [str(t) for t in Ts]
ax.bar(xs, stager, label="StageR (receiver: G+H+I)", color="#2ca02c")
ax.bar(xs, queueing, bottom=stager,
       label="Stage5 - StageR (worker spin + CXL prop + queueing)", color="#d62728")
ax.set_xlabel("T"); ax.set_ylabel("Latency (ns), log scale")
ax.set_yscale("log")
ax.set_title("Stage 5 (worker ack_wait) composition: receiver work vs queueing/spin", fontsize=11)
ax.grid(True, alpha=0.3, axis="y", which="both")
ax.legend(loc="upper left")
for i, (sr, q, s5) in enumerate(zip(stager, queueing, stage5)):
    ax.text(i, s5*1.1, f"{s5:.0f}ns", ha="center", fontsize=8)

# Panel 2: StageR/Stage5 % — shows "throat" engagement
ax = axes[1]
ax.plot(xs, sr_pct, marker="o", linewidth=2, color="#1f77b4")
ax.set_xlabel("T"); ax.set_ylabel("StageR / Stage5 (%)")
ax.set_title("Receiver work fraction of worker ack_wait — drops as workers queue up", fontsize=11)
ax.set_ylim(0, 100)
ax.axhline(50, color="gray", linestyle="--", alpha=0.5)
ax.grid(True, alpha=0.3)
for i, p in enumerate(sr_pct):
    ax.text(i, p+2, f"{p:.1f}%", ha="center", fontsize=8)

plt.tight_layout()
out = os.path.join(OUT_DIR, "Stage5_vs_StageR_composition.png")
plt.savefig(out, dpi=140); plt.close()
print(f"wrote {out}")

# Text summary
out = os.path.join(OUT_DIR, "Stage5_vs_StageR_summary.txt")
with open(out, "w") as f:
    f.write("Stage 5 composition: receiver work vs queueing\n")
    f.write("=" * 70 + "\n")
    f.write(f"{'T':>3} {'Stage5':>9} {'StageR':>7} {'queueing':>10} {'StageR/Stage5':>15}\n")
    for T, s5, sr, q, p in zip(Ts, stage5, stager, queueing, sr_pct):
        f.write(f"{T:>3} {s5:>9.0f} {sr:>7.0f} {q:>10.0f} {p:>14.1f}%\n")
print(f"wrote {out}")
