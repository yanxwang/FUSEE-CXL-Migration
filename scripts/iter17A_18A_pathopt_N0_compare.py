#!/usr/bin/env python3
"""Compare xhost_write vs xhost_read thpt(T) after path opt, single-recv N=0.

Sources:
  - iter-17A xhost_write path opt N=0  → 8group_median_table.csv row "iter-17A path opt N=0"
  - iter-18A xhost_read Phase 3 final  → phase3_final/grid.csv (build=p3_final)

Both: V=1024, zipf-0.99, N=0 (single ring + single receiver), cache=0,
per-host thpt (Mops/s, median across 3 reps).

Output: pathopt_N0_compare.png in current dir or args[3].
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = "/home/yanwang/FUSEE"
WRITE_CSV = f"{ROOT}/docs/iter17A_scaling_8group_final/8group_median_table.csv"
READ_CSV  = f"{ROOT}/docs/iter18A_phase3_final_20260523_060435/grid.csv"
OUT = f"{ROOT}/docs/iter17A_scaling_8group_final/pathopt_N0_write_vs_read.png"

# Load write thpt (Group 2 row, columns T=1..T=64)
write_thpt = {}
with open(WRITE_CSV) as f:
    rd = csv.DictReader(f)
    for r in rd:
        if r["Group"].startswith("2. iter-17A path opt N=0"):
            for k, v in r.items():
                if k.startswith("T="):
                    T = int(k[2:])
                    write_thpt[T] = float(v)
            break

# Load read thpt (median across reps per T from phase3 final)
read_thpt = defaultdict(list)
with open(READ_CSV) as f:
    rd = csv.DictReader(f)
    for r in rd:
        if r["build"] != "p3_final":
            continue
        T = int(r["T"])
        read_thpt[T].append(float(r["thpt"]) / 1e6)
read_thpt = {T: statistics.median(v) for T, v in read_thpt.items()}

Ts = sorted(set(write_thpt.keys()) | set(read_thpt.keys()))
w_vals = [write_thpt.get(T, 0) for T in Ts]
r_vals = [read_thpt.get(T, 0) for T in Ts]

fig, (ax_log, ax_lin) = plt.subplots(1, 2, figsize=(14, 5.5))

# Left: log
ax_log.plot(Ts, w_vals, marker="o", linewidth=2,
            label="xhost_write (iter-17A path opt N=0)", color="tab:red")
ax_log.plot(Ts, r_vals, marker="s", linewidth=2,
            label="xhost_read (iter-18A Phase 3 final N=0)", color="tab:blue")
ax_log.set_xscale("log", base=2)
ax_log.set_yscale("log")
ax_log.set_xticks(Ts); ax_log.set_xticklabels([str(t) for t in Ts])
ax_log.set_xlabel("T (workers per host)")
ax_log.set_ylabel("Per-host thpt (Mops/s, log)")
ax_log.set_title("Post-opt single-receiver thpt — log scale", fontsize=11)
ax_log.grid(True, alpha=0.3, which="both")
ax_log.legend(fontsize=10, loc="upper left")

# Right: linear
ax_lin.plot(Ts, w_vals, marker="o", linewidth=2,
            label="xhost_write (iter-17A path opt N=0)", color="tab:red")
ax_lin.plot(Ts, r_vals, marker="s", linewidth=2,
            label="xhost_read (iter-18A Phase 3 final N=0)", color="tab:blue")
ax_lin.set_xticks(Ts); ax_lin.set_xticklabels([str(t) for t in Ts])
ax_lin.set_xlabel("T (workers per host)")
ax_lin.set_ylabel("Per-host thpt (Mops/s, linear)")
ax_lin.set_title("Post-opt single-receiver thpt — linear scale", fontsize=11)
ax_lin.grid(True, alpha=0.3)
ax_lin.legend(fontsize=10, loc="upper left")

# Annotate peaks
wT = max(Ts, key=lambda T: write_thpt.get(T, 0))
rT = max(Ts, key=lambda T: read_thpt.get(T, 0))
ax_lin.annotate(f"write peak T={wT}: {write_thpt[wT]:.2f} Mops/host",
                xy=(wT, write_thpt[wT]),
                xytext=(wT - 18, write_thpt[wT] - 0.15),
                fontsize=9, color="tab:red",
                arrowprops=dict(arrowstyle="->", color="tab:red"))
ax_lin.annotate(f"read peak T={rT}: {read_thpt[rT]:.2f} Mops/host",
                xy=(rT, read_thpt[rT]),
                xytext=(rT - 24, read_thpt[rT] + 0.06),
                fontsize=9, color="tab:blue",
                arrowprops=dict(arrowstyle="->", color="tab:blue"))

fig.suptitle("xhost write vs read — single-ring + single-receiver, post path-opt "
             "(V=1024, zipf-0.99, cache=0)", fontsize=12, y=1.01)

plt.tight_layout()
plt.savefig(OUT, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {OUT}")

print()
print(f"{'T':>4} {'write':>10} {'read':>10} {'read/write':>12}")
for T in Ts:
    w = write_thpt.get(T, 0); r = read_thpt.get(T, 0)
    ratio = r / w if w > 0 else 0
    print(f"{T:>4} {w:>10.3f} {r:>10.3f} {ratio:>11.2f}x")
