#!/usr/bin/env python3
"""Plot Protocol F Fig 13 (YCSB throughput vs num clients).

Reads docs/protocol_F_fig13_<timestamp>/summary.csv and produces:
  fig13_thpt_vs_clients.png  - 4 lines (workload A/B/C/D) vs total_clients

Usage:
  python3 scripts/plot_protocol_F_fig13.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_fig13.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

# Aggregate by (workload, total_clients).
agg = defaultdict(int)
with open(CSV) as f:
    for r in csv.DictReader(f):
        wl = r["workload"]
        T = int(r["total_clients"])
        agg[(wl, T)] += int(r["trans_tpt"])

workloads = sorted({k[0] for k in agg.keys()})
all_T = sorted({k[1] for k in agg.keys()})

fig, ax = plt.subplots(figsize=(8, 5))
markers = {"a": "o", "b": "s", "c": "^", "d": "v"}
colors = {"a": "C0", "b": "C1", "c": "C2", "d": "C3"}
for wl in workloads:
    xs = [T for T in all_T if (wl, T) in agg]
    ys = [agg[(wl, T)] / 1e6 for T in xs]
    ax.plot(xs, ys, marker=markers.get(wl, "o"), color=colors.get(wl, "k"),
            label=f"YCSB-{wl.upper()}")
ax.set_xlabel("Total clients (g1 + g2)")
ax.set_ylabel("Aggregate cluster throughput (Mops/s)")
ax.set_title("FUSEE CXL Migration (LFM based) — Fig 13 YCSB throughput, 1024 B KV, Zipf 0.99")
ax.set_xscale("log", base=2)
ax.set_xticks(all_T)
ax.set_xticklabels([str(T) for T in all_T])
ax.grid(True, which="both", alpha=0.3)
ax.legend()
fig.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "fig13_thpt_vs_clients.png"), dpi=110)
print(f"wrote {OUT_DIR}/fig13_thpt_vs_clients.png")

md_path = os.path.join(OUT_DIR, "fig13_summary.md")
with open(md_path, "w") as fp:
    fp.write("| workload | " + " | ".join(f"T={T}" for T in all_T) + " |\n")
    fp.write("|" + "---|" * (len(all_T) + 1) + "\n")
    for wl in workloads:
        cells = [f"{agg[(wl, T)] / 1e6:.3f}" if (wl, T) in agg else "—" for T in all_T]
        fp.write(f"| YCSB-{wl.upper()} | " + " | ".join(cells) + " |\n")
print(f"wrote {md_path}")
