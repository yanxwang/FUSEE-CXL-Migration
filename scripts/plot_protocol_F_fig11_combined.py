#!/usr/bin/env python3
"""Fig 11 combined plot: left = throughput-vs-clients lines, right = peak bars.

Usage:
  python3 scripts/plot_protocol_F_fig11_combined.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_fig11_combined.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

agg = defaultdict(lambda: {"insert": 0, "search": 0, "update": 0, "delete": 0})
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["total_clients"])
        for op in ("insert", "search", "update", "delete"):
            agg[T][op] += int(r[f"{op}_tpt"])

Ts = sorted(agg.keys())
ops = ["search", "insert", "update", "delete"]
colors = {"search": "tab:blue", "insert": "tab:red",
          "update": "tab:orange", "delete": "tab:green"}
markers = {"search": "s", "insert": "o", "update": "^", "delete": "v"}

def m(op): return [agg[T][op] / 1e6 for T in Ts]

# Peaks
peaks = {op: max(agg[T][op] for T in Ts) / 1e6 for op in ops}
peaks_at = {op: max(Ts, key=lambda T: agg[T][op]) for op in ops}

fig, (ax_l, ax_r) = plt.subplots(1, 2, figsize=(15, 5),
                                 gridspec_kw={"width_ratios": [1.4, 1]})

# Left: line plot
for op in ops:
    ax_l.plot(Ts, m(op), marker=markers[op], color=colors[op],
              label=op.upper(), linewidth=2, markersize=7)
ax_l.set_xlabel("Total clients (g1 + g2)")
ax_l.set_ylabel("Aggregate throughput (Mops/s)")
ax_l.set_title("(a) Throughput vs num clients")
ax_l.set_xscale("log", base=2)
ax_l.set_xticks(Ts); ax_l.set_xticklabels([str(T) for T in Ts])
ax_l.grid(True, which="both", alpha=0.3)
ax_l.legend(loc="upper left")

# Right: peak bars
x = list(range(len(ops)))
vals = [peaks[op] for op in ops]
bars = ax_r.bar(x, vals, color=[colors[op] for op in ops], edgecolor="black")
for bar, op in zip(bars, ops):
    h = bar.get_height()
    ax_r.text(bar.get_x() + bar.get_width() / 2, h * 1.02,
              f"{h:.2f}\n(T={peaks_at[op]})",
              ha="center", va="bottom", fontsize=10)
ax_r.set_xticks(x); ax_r.set_xticklabels([op.upper() for op in ops])
ax_r.set_ylabel("Peak Mops/s")
ax_r.set_xlabel("Operation")
ax_r.set_title("(b) Peak per-op throughput")
ax_r.set_ylim(0, max(vals) * 1.20)
ax_r.grid(True, axis="y", alpha=0.3)

fig.suptitle("FUSEE CXL Migration (LFM based) — Fig 11 (1024 B KV)",
             fontsize=13, y=1.02)
fig.tight_layout()
out = os.path.join(OUT_DIR, "fig11_combined.png")
plt.savefig(out, dpi=130, bbox_inches="tight")
print(f"wrote {out}")
