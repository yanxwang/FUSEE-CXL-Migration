#!/usr/bin/env python3
"""Fig 13 combined plot: left = throughput-vs-clients lines (per workload),
right = peak bars (per workload).

Usage:
  python3 scripts/plot_protocol_F_fig13_combined.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_fig13_combined.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

agg = defaultdict(int)
with open(CSV) as f:
    for r in csv.DictReader(f):
        wl = r["workload"]
        T = int(r["total_clients"])
        agg[(wl, T)] += int(r["trans_tpt"])

workloads = sorted({k[0] for k in agg.keys()})
Ts = sorted({k[1] for k in agg.keys()})
markers = {"a": "o", "b": "s", "c": "^", "d": "v"}
colors  = {"a": "tab:red", "b": "tab:orange", "c": "tab:blue", "d": "tab:green"}

peaks   = {wl: max(agg[(wl, T)] for T in Ts if (wl, T) in agg) / 1e6 for wl in workloads}
peaks_at = {wl: max((T for T in Ts if (wl, T) in agg),
                    key=lambda T: agg[(wl, T)]) for wl in workloads}

fig, (ax_l, ax_r) = plt.subplots(1, 2, figsize=(15, 5),
                                 gridspec_kw={"width_ratios": [1.4, 1]})

# Left: line plot
for wl in workloads:
    xs = [T for T in Ts if (wl, T) in agg]
    ys = [agg[(wl, T)] / 1e6 for T in xs]
    ax_l.plot(xs, ys, marker=markers.get(wl, "o"), color=colors.get(wl, "k"),
              label=f"YCSB-{wl.upper()}", linewidth=2, markersize=7)
ax_l.set_xlabel("Total clients (g1 + g2)")
ax_l.set_ylabel("Aggregate throughput (Mops/s)")
ax_l.set_title("(a) Throughput vs num clients")
ax_l.set_xscale("log", base=2)
ax_l.set_xticks(Ts); ax_l.set_xticklabels([str(T) for T in Ts])
ax_l.grid(True, which="both", alpha=0.3)
ax_l.legend(loc="upper left")

# Right: peak bars
x = list(range(len(workloads)))
vals = [peaks[wl] for wl in workloads]
bars = ax_r.bar(x, vals,
                color=[colors.get(wl, "k") for wl in workloads],
                edgecolor="black")
for bar, wl in zip(bars, workloads):
    h = bar.get_height()
    ax_r.text(bar.get_x() + bar.get_width() / 2, h * 1.02,
              f"{h:.2f}\n(T={peaks_at[wl]})",
              ha="center", va="bottom", fontsize=10)
ax_r.set_xticks(x); ax_r.set_xticklabels([f"YCSB-{wl.upper()}" for wl in workloads])
ax_r.set_ylabel("Peak Mops/s")
ax_r.set_xlabel("Workload")
ax_r.set_title("(b) Peak per-workload throughput")
ax_r.set_ylim(0, max(vals) * 1.20)
ax_r.grid(True, axis="y", alpha=0.3)

fig.suptitle("FUSEE CXL Migration (LFM based) — Fig 13 YCSB (1024 B KV, Zipf 0.99)",
             fontsize=13, y=1.02)
fig.tight_layout()
out = os.path.join(OUT_DIR, "fig13_combined.png")
plt.savefig(out, dpi=130, bbox_inches="tight")
print(f"wrote {out}")
