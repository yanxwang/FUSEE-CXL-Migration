#!/usr/bin/env python3
"""iter-18A Phase 2.1 baseline vs Phase 3 final post-opt thpt vs T.

Plots cluster Mops/s side-by-side: log scale (left) + linear (right).

Usage:
  python3 scripts/iter18A_phase3_thpt_plot.py \
    docs/iter18A_phase2_T_sweep_20260523_044157/grid.csv \
    docs/iter18A_phase3_final_20260523_060435/grid.csv

Output: phase3_thpt_vs_T.png in Phase 3 final dir.
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 3:
    print("Usage: iter18A_phase3_thpt_plot.py <P2.1_grid.csv> <P3_final_grid.csv>")
    sys.exit(1)
P21_CSV = sys.argv[1]
P3_CSV = sys.argv[2]
OUT_DIR = os.path.dirname(os.path.abspath(P3_CSV))

def load_thpt(path, build_filter):
    """build_filter selects which 'build' column rows; returns {T: median cluster_thpt_Mops}."""
    by_T = defaultdict(list)
    with open(path) as f:
        for r in csv.DictReader(f):
            if r.get("build", "") != build_filter:
                continue
            try:
                T = int(r["T"])
                thpt = float(r["thpt"])
                # thpt column is per-host total ops/s; cluster = 2× single-host
                # (probe-off / Phase 3 final both report per-host trans_agg_thpt)
                by_T[T].append(thpt / 1e6)  # per-host (matches iter-18A summary doc)
            except (ValueError, KeyError):
                pass
    return {T: statistics.median(v) for T, v in by_T.items() if v}

baseline = load_thpt(P21_CSV, "off")  # Phase 2.1 probe-off baseline
p3final = load_thpt(P3_CSV, "p3_final")

Ts = sorted(set(baseline.keys()) | set(p3final.keys()))
b_vals = [baseline.get(T, 0) for T in Ts]
p_vals = [p3final.get(T, 0) for T in Ts]

fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(14, 5.5))

# Left: log-log
ax_log.plot(Ts, b_vals, marker="o", linewidth=2, label="Phase 2.1 baseline (pre-opt)",
            color="tab:gray")
ax_log.plot(Ts, p_vals, marker="s", linewidth=2,
            label="Phase 3 final (post-opt, N=0)", color="tab:blue")
ax_log.set_xscale("log", base=2)
ax_log.set_yscale("log")
ax_log.set_xticks(Ts)
ax_log.set_xticklabels([str(t) for t in Ts])
ax_log.set_xlabel("T (workers per host)")
ax_log.set_ylabel("Per-host thpt (Mops/s, log)")
ax_log.set_title("XR per-host thpt vs T — log scale", fontsize=11)
ax_log.grid(True, alpha=0.3, which="both")
ax_log.legend(fontsize=10, loc="upper left")

# Right: linear
ax_lin.plot(Ts, b_vals, marker="o", linewidth=2, label="Phase 2.1 baseline (pre-opt)",
            color="tab:gray")
ax_lin.plot(Ts, p_vals, marker="s", linewidth=2,
            label="Phase 3 final (post-opt, N=0)", color="tab:blue")
ax_lin.set_xticks(Ts)
ax_lin.set_xticklabels([str(t) for t in Ts])
ax_lin.set_xlabel("T (workers per host)")
ax_lin.set_ylabel("Per-host thpt (Mops/s, linear)")
ax_lin.set_title("XR per-host thpt vs T — linear scale", fontsize=11)
ax_lin.grid(True, alpha=0.3)
ax_lin.legend(fontsize=10, loc="upper left")

# Annotate gain at T=32 (peak)
peak_T = max(Ts, key=lambda T: p3final.get(T, 0))
if peak_T in baseline and peak_T in p3final and baseline[peak_T] > 0:
    gain = (p3final[peak_T] - baseline[peak_T]) / baseline[peak_T] * 100
    ax_lin.annotate(f"peak T={peak_T}: {p3final[peak_T]:.2f} Mops/host\n+{gain:.0f}% vs baseline",
                    xy=(peak_T, p3final[peak_T]),
                    xytext=(peak_T - 8, p3final[peak_T] * 0.65),
                    fontsize=9,
                    arrowprops=dict(arrowstyle="->", color="black"))

fig.suptitle("iter-18A xhost_read cumulative path opt effect (V=1024, zipf-0.99, N=0, cache=0)",
             fontsize=12, y=1.01)

out = os.path.join(OUT_DIR, "phase3_thpt_vs_T.png")
plt.tight_layout()
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")

# Also dump a summary table
print()
print(f"{'T':>4} {'baseline':>10} {'P3 final':>10} {'gain':>8}")
for T in Ts:
    b = baseline.get(T, 0)
    p = p3final.get(T, 0)
    g = (p - b) / b * 100 if b > 0 else 0
    print(f"{T:>4} {b:>10.3f} {p:>10.3f} {g:>7.0f}%")
