#!/usr/bin/env python3
"""Plot Protocol A Fig 11 (per-op throughput vs num clients).

Reads docs/protocol_A_fig11_<timestamp>/summary.csv and produces:
  fig11_thpt_vs_clients.png  - 4 lines (insert/search/update/delete) vs total_clients

NOTE: For Protocol A, the SUMMARY line printed by g1's primary already aggregates
across BOTH hosts via the WorkerStats array (each worker publishes its per-phase
count to shared CXL stats). The summary.csv only contains g1's row per cell
(g2 prints no SUMMARY since it isn't the primary). So we use the g1 row directly
as the cluster total.

Usage:
  python3 scripts/plot_protocol_A_fig11.py <summary.csv>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_A_fig11.py <summary.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

agg = {}
with open(CSV) as f:
    for r in csv.DictReader(f):
        if r.get("host") != "g1":
            continue
        T = int(r["total_clients"])
        agg[T] = {
            "ins": int(r["insert_tpt"]),
            "sea": int(r["search_tpt"]),
            "upd": int(r["update_tpt"]),
            "del": int(r["delete_tpt"]),
        }

Ts = sorted(agg.keys())

def to_mops(x): return x / 1e6
ins  = [to_mops(agg[T]["ins"]) for T in Ts]
sea  = [to_mops(agg[T]["sea"]) for T in Ts]
upd  = [to_mops(agg[T]["upd"]) for T in Ts]
dele = [to_mops(agg[T]["del"]) for T in Ts]

fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(Ts, ins,  "o-", label="INSERT", color="C0")
ax.plot(Ts, sea,  "s-", label="SEARCH", color="C1")
ax.plot(Ts, upd,  "^-", label="UPDATE", color="C2")
ax.plot(Ts, dele, "v-", label="DELETE", color="C3")
ax.set_xlabel("Total clients (g1 + g2)")
ax.set_ylabel("Aggregate cluster throughput (Mops/s)")
ax.set_title("FUSEE-CXL Protocol A (per-host bucket partition) — Fig 11 per-op throughput, 256 B KV")
ax.set_xscale("log", base=2)
ax.set_xticks(Ts)
ax.set_xticklabels([str(T) for T in Ts])
ax.grid(True, which="both", alpha=0.3)
ax.legend()
fig.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "fig11_thpt_vs_clients.png"), dpi=110)
print(f"wrote {OUT_DIR}/fig11_thpt_vs_clients.png")

md_path = os.path.join(OUT_DIR, "fig11_summary.md")
with open(md_path, "w") as fp:
    fp.write("# Protocol A Fig 11 summary (cluster aggregate, Mops/s)\n\n")
    fp.write("| total_clients | INSERT | SEARCH | UPDATE | DELETE |\n")
    fp.write("|---|---|---|---|---|\n")
    for T in Ts:
        a = agg[T]
        fp.write(f"| {T} | {a['ins']/1e6:.2f} | {a['sea']/1e6:.2f} | {a['upd']/1e6:.2f} | {a['del']/1e6:.2f} |\n")
print(f"wrote {md_path}")
