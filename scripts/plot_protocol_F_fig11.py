#!/usr/bin/env python3
"""Plot Protocol F Fig 11 (per-op throughput vs num clients).

Reads docs/protocol_F_fig11_<timestamp>/summary.csv and produces:
  fig11_thpt_vs_clients.png  - 4 lines (insert/search/update/delete) vs total_clients

Usage:
  python3 scripts/plot_protocol_F_fig11.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_fig11.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

# Aggregate cluster throughput: sum over hosts for each (total_clients).
agg = defaultdict(lambda: {"ins": 0, "sea": 0, "upd": 0, "del": 0})
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["total_clients"])
        agg[T]["ins"] += int(r["insert_tpt"])
        agg[T]["sea"] += int(r["search_tpt"])
        agg[T]["upd"] += int(r["update_tpt"])
        agg[T]["del"] += int(r["delete_tpt"])

Ts = sorted(agg.keys())

# Convert to Mops/s for plot.
def to_mops(x): return x / 1e6
ins = [to_mops(agg[T]["ins"]) for T in Ts]
sea = [to_mops(agg[T]["sea"]) for T in Ts]
upd = [to_mops(agg[T]["upd"]) for T in Ts]
dele = [to_mops(agg[T]["del"]) for T in Ts]

fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(Ts, ins, "o-", label="INSERT", color="C0")
ax.plot(Ts, sea, "s-", label="SEARCH", color="C1")
ax.plot(Ts, upd, "^-", label="UPDATE", color="C2")
ax.plot(Ts, dele, "v-", label="DELETE", color="C3")
ax.set_xlabel("Total clients (g1 + g2, each host runs N/2)")
ax.set_ylabel("Aggregate cluster throughput (Mops/s)")
ax.set_title("FUSEE CXL Migration (LFM based) — Fig 11 per-op throughput, 1024 B KV")
ax.set_xscale("log", base=2)
ax.set_xticks(Ts)
ax.set_xticklabels([str(T) for T in Ts])
ax.grid(True, which="both", alpha=0.3)
ax.legend()
fig.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "fig11_thpt_vs_clients.png"), dpi=110)
print(f"wrote {OUT_DIR}/fig11_thpt_vs_clients.png")

# Write a markdown table for the alignment doc.
md_path = os.path.join(OUT_DIR, "fig11_summary.md")
with open(md_path, "w") as fp:
    fp.write("| total clients | INSERT (Mops/s) | SEARCH | UPDATE | DELETE |\n")
    fp.write("|---|---|---|---|---|\n")
    for T in Ts:
        fp.write(f"| {T} | {to_mops(agg[T]['ins']):.3f} | "
                 f"{to_mops(agg[T]['sea']):.3f} | "
                 f"{to_mops(agg[T]['upd']):.3f} | "
                 f"{to_mops(agg[T]['del']):.3f} |\n")
print(f"wrote {md_path}")
