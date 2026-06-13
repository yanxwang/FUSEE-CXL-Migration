#!/usr/bin/env python3
"""4-line plot: write/read × pre/post path opt, all N=0 single receiver.

Sources (all per-host Mops, V=1024, zipf-0.99, cache=0):
  - write pre-opt  = iter-15A baseline                 [8group_median_table row 1]
  - write post-opt = iter-17A path opt N=0             [8group_median_table row 2]
  - read pre-opt   = iter-18A Phase 2.1 baseline       [phase2 T_sweep grid.csv build=off]
  - read post-opt  = iter-18A Phase 3 final            [phase3_final grid.csv build=p3_final]

Output: 4line_pre_vs_post_opt.png in iter17A 8group dir.
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = "/home/yanwang/FUSEE"
WRITE_TABLE = f"{ROOT}/docs/iter17A_scaling_8group_final/8group_median_table.csv"
READ_PRE    = f"{ROOT}/docs/iter18A_phase2_T_sweep_20260523_044157/grid.csv"
READ_POST   = f"{ROOT}/docs/iter18A_phase3_final_20260523_060435/grid.csv"
OUT = f"{ROOT}/docs/iter17A_scaling_8group_final/4line_pre_vs_post_opt.png"

# Helpers
def row_to_thpt(row):
    """Group row → {T: float Mops}."""
    out = {}
    for k, v in row.items():
        if k.startswith("T="):
            try: out[int(k[2:])] = float(v)
            except ValueError: pass
    return out

def grid_median(csv_path, build_value):
    by_T = defaultdict(list)
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            if r.get("build") != build_value: continue
            try:
                T = int(r["T"]); thpt = float(r["thpt"])
                by_T[T].append(thpt / 1e6)
            except (ValueError, KeyError): pass
    return {T: statistics.median(v) for T, v in by_T.items() if v}

write_pre, write_post = {}, {}
with open(WRITE_TABLE) as f:
    for r in csv.DictReader(f):
        if r["Group"].startswith("1. iter-15A baseline"):
            write_pre = row_to_thpt(r)
        elif r["Group"].startswith("2. iter-17A path opt N=0"):
            write_post = row_to_thpt(r)

read_pre  = grid_median(READ_PRE, "off")
read_post = grid_median(READ_POST, "p3_final")

Ts = sorted(set(write_pre) | set(write_post) | set(read_pre) | set(read_post))
def vals(d): return [d.get(T, 0) for T in Ts]

fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(14, 5.5))

styles = [
    ("write pre-opt  (iter-15A baseline)",       "tab:red",   "o", "--", write_pre),
    ("write post-opt (iter-17A path opt N=0)",   "tab:red",   "o", "-",  write_post),
    ("read  pre-opt  (iter-18A Phase 2.1 baseline)", "tab:blue", "s", "--", read_pre),
    ("read  post-opt (iter-18A Phase 3 final N=0)",  "tab:blue", "s", "-",  read_post),
]

for label, color, marker, ls, d in styles:
    ax_log.plot(Ts, vals(d), marker=marker, linewidth=2, linestyle=ls,
                color=color, label=label)
    ax_lin.plot(Ts, vals(d), marker=marker, linewidth=2, linestyle=ls,
                color=color, label=label)

ax_log.set_xscale("log", base=2); ax_log.set_yscale("log")
ax_log.set_xticks(Ts); ax_log.set_xticklabels([str(t) for t in Ts])
ax_log.set_xlabel("T (workers per host)")
ax_log.set_ylabel("Per-host thpt (Mops/s, log)")
ax_log.set_title("Pre vs post path-opt thpt vs T — log scale", fontsize=11)
ax_log.grid(True, alpha=0.3, which="both")
ax_log.legend(fontsize=8, loc="upper left")

ax_lin.set_xticks(Ts); ax_lin.set_xticklabels([str(t) for t in Ts])
ax_lin.set_xlabel("T (workers per host)")
ax_lin.set_ylabel("Per-host thpt (Mops/s, linear)")
ax_lin.set_title("Pre vs post path-opt thpt vs T — linear scale", fontsize=11)
ax_lin.grid(True, alpha=0.3)
ax_lin.legend(fontsize=8, loc="upper left")

fig.suptitle("xhost write/read × pre/post path-opt — single ring + single receiver "
             "(V=1024, zipf-0.99, cache=0)", fontsize=12, y=1.01)

plt.tight_layout()
plt.savefig(OUT, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {OUT}")
print()
print(f"{'T':>4} {'w_pre':>9} {'w_post':>9} {'w_gain':>8}  "
      f"{'r_pre':>9} {'r_post':>9} {'r_gain':>8}")
for T in Ts:
    wp = write_pre.get(T, 0); wq = write_post.get(T, 0)
    rp = read_pre.get(T, 0); rq = read_post.get(T, 0)
    wg = (wq-wp)/wp*100 if wp else 0
    rg = (rq-rp)/rp*100 if rp else 0
    print(f"{T:>4} {wp:>9.3f} {wq:>9.3f} {wg:>7.0f}%  "
          f"{rp:>9.3f} {rq:>9.3f} {rg:>7.0f}%")
