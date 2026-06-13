#!/usr/bin/env python3
"""Plot CXL cross-host write atomicity study results.

Reads the merged CSV from all phase{1,2,3} sweeps under
docs/study_cxl_write_atomicity/ and produces:

  - atomicity_chart.png — INTERLEAVE rate vs N, by (store, mode) variant
    with the 3-CL atomic boundary annotated.

Usage:
  python3 scripts/plot_cxl_atomicity_study.py
"""
import csv, glob, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

STUDY_ROOT = "docs/study_cxl_write_atomicity"
OUT_PATH = os.path.join(STUDY_ROOT, "atomicity_chart.png")

# Collect all (n, store, mode) -> (K, INTL) from all raw.csv.
# Prefer the latest cell for each (n, store, fence, mode) key — verification
# cells (K=10000) win over baseline (K=1000).
records = {}
for csv_path in sorted(glob.glob(f"{STUDY_ROOT}/phase*_*/raw.csv")):
    with open(csv_path) as f:
        r = csv.DictReader(f)
        for row in r:
            # Phase 1's older CSV lacks the 'mode' column; default to barrier.
            try:
                n = int(row["n"])
                K = int(row["K"])
                intl_str = row.get("INTERLEAVE")
                if not intl_str:
                    continue
                intl = int(intl_str)
            except (KeyError, ValueError, TypeError):
                continue
            store = row.get("store", "?")
            mode = row.get("mode") or "barrier"
            fence = row.get("fence", "?")
            align = row.get("align", "aligned")
            # Only the most interesting variant: aligned, clflush_sfence,
            # same_cl. Skip exotic combos.
            if align != "aligned" or fence != "clflush_sfence":
                continue
            key = (n, store, mode)
            # Keep highest K (verification > baseline).
            if key not in records or records[key][0] < K:
                records[key] = (K, intl)

# Series keyed by (store, mode).
series = defaultdict(list)
for (n, store, mode), (K, intl) in records.items():
    series[(store, mode)].append((n, K, intl))

for k in series:
    series[k].sort(key=lambda x: x[0])

fig, ax = plt.subplots(figsize=(10, 6))
style = {
    ("memcpy", "barrier"): ("o-", "tab:red",    "memcpy + clflushopt + sfence (barrier)"),
    ("memcpy", "async"):   ("o--", "tab:orange", "memcpy + clflushopt + sfence (async)"),
    ("movnti", "barrier"): ("s-", "tab:blue",   "movnti + sfence (barrier)"),
    ("movnti", "async"):   ("s--", "tab:cyan",  "movnti + sfence (async)"),
}

for key, pts in series.items():
    if key not in style:
        continue
    ls, color, label = style[key]
    ns = [p[0] for p in pts]
    intl_pct = [100.0 * p[2] / p[1] for p in pts]
    # Mark verified-K=10K with a star.
    K_max = max(p[1] for p in pts)
    label_full = f"{label} (max K = {K_max})"
    ax.plot(ns, intl_pct, ls, color=color, label=label_full,
            linewidth=2, markersize=7)

# Atomic-boundary annotation: vertical line at N=192 (3 cachelines).
ax.axvline(192, color="black", linestyle=":", alpha=0.5,
           label="3-CL atomic boundary (N=192)")
ax.text(195, 25, "3 CL ←→ 4 CL\nstrict-atomic boundary",
        fontsize=9, alpha=0.7)

# Cacheline annotations at top.
for n_mark in [64, 128, 192, 256, 512, 1024]:
    cl = (n_mark + 63) // 64
    ax.annotate(f"{cl} CL", xy=(n_mark, 45), ha="center",
                fontsize=7, alpha=0.6, color="gray")

ax.set_xscale("log", base=2)
ax.set_xticks([1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 512, 1024])
ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64", "128", "192",
                    "256", "512", "1024"])
ax.set_xlabel("Write unit size (bytes)")
ax.set_ylabel("INTERLEAVE rate (%)")
ax.set_ylim(-2, 50)
ax.set_xlim(0.7, 1500)
ax.grid(True, alpha=0.3, which="both")
ax.legend(loc="upper left", fontsize=9)
ax.set_title("CXL cross-host write atomicity on g1/g2 — INTERLEAVE rate vs N\n"
             "(2-host XConn-switched CXL Type-3, kernel 6.15.0-uintr)",
             fontsize=11)
fig.tight_layout()
fig.savefig(OUT_PATH, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {OUT_PATH}")

# Companion table.
table_path = os.path.join(STUDY_ROOT, "atomicity_summary_table.md")
with open(table_path, "w") as fp:
    fp.write("# CXL write atomicity — summary table\n\n")
    fp.write("Source: highest-K cell per (n, store, mode) across all sweep CSVs.\n\n")
    fp.write("| N (B) | CL | memcpy barrier (INTL/K) | memcpy async | movnti barrier | movnti async |\n")
    fp.write("|---|---|---|---|---|---|\n")
    ns_sorted = sorted(set(n for (n, _, _) in records.keys()))
    for n in ns_sorted:
        cl = (n + 63) // 64
        cells = []
        for store, mode in [("memcpy", "barrier"), ("memcpy", "async"),
                             ("movnti", "barrier"), ("movnti", "async")]:
            v = records.get((n, store, mode))
            if v is None:
                cells.append("—")
            else:
                K, intl = v
                cells.append(f"{intl} / {K}")
        fp.write(f"| {n} | {cl} | {cells[0]} | {cells[1]} | {cells[2]} | {cells[3]} |\n")
print(f"wrote {table_path}")
