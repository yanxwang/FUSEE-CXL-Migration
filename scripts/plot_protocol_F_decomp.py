#!/usr/bin/env python3
"""Plot Protocol F stage-decomposition microbench results.

Reads results/decomp_stages-Fp.csv (output of cxl_kv_ops_F_microbench built
with -DFUSEE_F_DECOMP=ON) and produces:
  decomp_stages.png  - stacked-bar showing p50 ns contribution of each
                       stage per op type, plus TOTAL line.
  decomp_table.md    - markdown table per op type.

Usage:
  python3 scripts/plot_protocol_F_decomp.py <results_dir>
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_decomp.py <results_dir>")
    sys.exit(1)
RDIR = sys.argv[1]
CSV  = os.path.join(RDIR, "decomp_stages-Fp.csv")

# Parse: rows are (op, stage, count, avg_ns, p50, p90, p99, p999, min, max)
rows = []
with open(CSV) as f:
    rd = csv.reader(f)
    for r in rd:
        if not r or r[0] == "stage": continue  # skip embedded headers
        # The dump uses prefix "<op>," then "stage,count,..." line per stage
        op    = r[0]
        stage = r[1]
        if stage == "stage": continue  # header row from f_decomp_dump_stages
        try:
            n     = int(r[2])
            avg   = int(r[3])
            p50   = int(r[4])
            p90   = int(r[5])
            p99   = int(r[6])
            p999  = int(r[7])
            mn    = int(r[8])
            mx    = int(r[9])
        except (ValueError, IndexError):
            continue
        rows.append({"op": op, "stage": stage, "n": n,
                     "avg": avg, "p50": p50, "p90": p90,
                     "p99": p99, "p999": p999, "min": mn, "max": mx})

# Group by op type. Stage names are prefixed by op type (INS_, UPD_, DEL_, SRC_)
# so we filter only the matching stages for each op.
OP_TO_PREFIX = {"insert": "INS_", "update": "UPD_", "delete": "DEL_", "search": "SRC_"}

# Stage ordering within each op type (excluding TOTAL).
STAGE_ORDER = {
    "insert": ["INS_pre_flush", "INS_pre_scan", "INS_alloc",
               "INS_write_pair", "INS_lock", "INS_verify",
               "INS_publish", "INS_unlock", "INS_cache"],
    "update": ["UPD_flush_scan", "UPD_alloc_write", "UPD_lock",
               "UPD_verify", "UPD_publish", "UPD_unlock",
               "UPD_free", "UPD_cache"],
    "delete": ["DEL_flush_scan", "DEL_lock", "DEL_verify",
               "DEL_clear", "DEL_unlock", "DEL_free", "DEL_cache"],
    "search": ["SRC_cache_lookup", "SRC_fast_path", "SRC_slow_path",
               "SRC_cache_update"],
}

# Build per-op stage_p50 dict
op_data = {}
totals  = {}
for op, prefix in OP_TO_PREFIX.items():
    d = {}
    total = 0
    for r in rows:
        if r["op"] != op: continue
        s = r["stage"]
        if not s.startswith(prefix): continue
        if s.endswith("_TOTAL"):
            totals[op] = r
        else:
            d[s] = r
    op_data[op] = d

# --- Stacked bar chart of p50 contributions ---
fig, ax = plt.subplots(figsize=(11, 6.5))
op_labels = ["insert", "update", "delete", "search"]
bar_w = 0.6
xs = list(range(len(op_labels)))

# Stage palette (cycled)
palette = plt.cm.tab20.colors

# For each op, stack the stages in STAGE_ORDER
for i, op in enumerate(op_labels):
    bottom = 0
    stages = STAGE_ORDER[op]
    for j, s in enumerate(stages):
        if s not in op_data[op]: continue
        p50 = op_data[op][s]["p50"]
        color = palette[j % len(palette)]
        ax.bar(i, p50, bottom=bottom, width=bar_w, color=color,
               edgecolor="white", linewidth=0.5)
        # Annotate stage label and value inside the bar (if room)
        if p50 > 200:  # skip annotations for tiny stages
            ax.text(i, bottom + p50/2, f"{s.split('_',1)[1]}\n{p50}ns",
                    ha="center", va="center", fontsize=7, color="black")
        bottom += p50
    # Annotate total
    total = totals.get(op, {}).get("p50", bottom)
    ax.scatter([i], [total], color="red", marker="_", s=200, zorder=5)
    ax.text(i, total + 200, f"TOTAL p50={total}ns",
            ha="center", va="bottom", fontsize=9, color="red", fontweight="bold")

ax.set_xticks(xs)
ax.set_xticklabels([op.upper() for op in op_labels])
ax.set_ylabel("Latency contribution (ns, p50)")
ax.set_title("Protocol F per-stage latency decomposition (single-client, 100K ops, g1+g2)")
ax.grid(True, alpha=0.3, axis="y")

out = os.path.join(RDIR, "decomp_stages.png")
fig.tight_layout()
fig.savefig(out, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {out}")

# --- Markdown table ---
md_out = os.path.join(RDIR, "decomp_table.md")
with open(md_out, "w") as f:
    f.write("# Protocol F stage-decomposition microbench summary\n\n")
    for op in op_labels:
        prefix = OP_TO_PREFIX[op]
        f.write(f"## {op.upper()}\n\n")
        f.write("| stage | count | avg_ns | p50_ns | p90_ns | p99_ns | p999_ns | min_ns | max_ns |\n")
        f.write("|---|---|---|---|---|---|---|---|---|\n")
        for s in STAGE_ORDER[op]:
            if s not in op_data[op]: continue
            r = op_data[op][s]
            f.write(f"| {s} | {r['n']} | {r['avg']} "
                    f"| {r['p50']} | {r['p90']} | {r['p99']} | {r['p999']} "
                    f"| {r['min']} | {r['max']} |\n")
        # TOTAL
        if op in totals:
            r = totals[op]
            f.write(f"| **TOTAL** | {r['n']} | **{r['avg']}** "
                    f"| **{r['p50']}** | {r['p90']} | {r['p99']} | {r['p999']} "
                    f"| {r['min']} | {r['max']} |\n")
        # Sum of stage p50 vs TOTAL (gap = overhead, e.g., timestamp calls)
        sum_p50 = sum(op_data[op][s]["p50"] for s in STAGE_ORDER[op]
                       if s in op_data[op])
        total_p50 = totals.get(op, {}).get("p50", 0)
        f.write(f"\n*Sum of stages p50 = {sum_p50} ns; TOTAL p50 = {total_p50} ns; "
                f"gap = {total_p50 - sum_p50} ns (timestamp + un-instrumented).*\n\n")
print(f"wrote {md_out}")
