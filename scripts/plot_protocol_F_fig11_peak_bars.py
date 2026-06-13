#!/usr/bin/env python3
"""Plot peak per-op throughput bar chart (FUSEE paper Fig 11 layout).

Reads docs/protocol_F_fig11_<ts>/summary.csv and emits a 4-bar chart
with the PEAK aggregate cluster throughput observed for each op type
across the client-count sweep — matches FUSEE paper Fig 11's bar chart.

Usage:
  python3 scripts/plot_protocol_F_fig11_peak_bars.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_fig11_peak_bars.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

# Aggregate cluster throughput per (total_clients) — sum over hosts.
agg = defaultdict(lambda: {"insert": 0, "search": 0, "update": 0, "delete": 0})
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["total_clients"])
        for op in ("insert", "search", "update", "delete"):
            agg[T][op] += int(r[f"{op}_tpt"])

# Find peak per op across all T values.
ops = ["search", "insert", "update", "delete"]   # paper Fig 11 ordering
peaks = {}
peaks_at = {}
for op in ops:
    best_T, best_v = max(((T, agg[T][op]) for T in agg.keys()), key=lambda kv: kv[1])
    peaks[op]    = best_v / 1e6
    peaks_at[op] = best_T

print("PEAK per op:")
for op in ops:
    print(f"  {op:<7} {peaks[op]:6.3f} Mops/s @ T={peaks_at[op]}")

# Bar chart — match FUSEE Fig 11 layout (one bar per op, all same color).
fig, ax = plt.subplots(figsize=(8, 5))
colors = {"search": "tab:blue", "insert": "tab:red",
          "update": "tab:orange", "delete": "tab:green"}
x = list(range(len(ops)))
vals = [peaks[op] for op in ops]
bars = ax.bar(x, vals, color=[colors[op] for op in ops], edgecolor="black")
for bar, op in zip(bars, ops):
    h = bar.get_height()
    ax.text(bar.get_x() + bar.get_width() / 2, h * 1.02,
            f"{h:.2f}\n(T={peaks_at[op]})",
            ha="center", va="bottom", fontsize=10)
ax.set_xticks(x)
ax.set_xticklabels([op.upper() for op in ops])
ax.set_ylabel("Aggregate cluster throughput (Mops/s)")
ax.set_xlabel("Operation")
ax.set_title("FUSEE CXL Migration (LFM based) — Fig 11 peak per-op throughput, 1024 B KV")
ax.set_ylim(0, max(vals) * 1.20 if vals else 1)
ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout()
out = os.path.join(OUT_DIR, "fig11_peak_bars.png")
plt.savefig(out, dpi=130)
print(f"wrote {out}")

# Markdown peak summary alongside the bar.
md = os.path.join(OUT_DIR, "fig11_peak_summary.md")
with open(md, "w") as fp:
    fp.write("| op | peak Mops/s | at T= |\n|---|---|---|\n")
    for op in ops:
        fp.write(f"| {op.upper()} | {peaks[op]:.3f} | {peaks_at[op]} |\n")
print(f"wrote {md}")
