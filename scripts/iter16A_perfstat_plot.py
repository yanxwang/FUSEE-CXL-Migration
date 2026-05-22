#!/usr/bin/env python3
"""iter-16A perfstat T-sweep plotter."""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

by_T = defaultdict(lambda: defaultdict(list))
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"])
        for k, v in r.items():
            if k in ("T", "rep", "scenario"): continue
            try: by_T[T][k].append(float(v))
            except: pass

Ts = sorted(by_T.keys())
med = lambda T, k: statistics.median(by_T[T].get(k, [0])) if by_T[T].get(k) else 0

# cycles/op, IPC, LLC miss%, cache_miss/op vs T
fig, axes = plt.subplots(2, 2, figsize=(11, 7))

# cycles/op
ax = axes[0,0]
vals = [med(T, "cycles")/5e6 for T in Ts]
ax.plot(Ts, vals, marker="o", linewidth=2, color="#1f77b4")
ax.set_xscale("log", base=2); ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T"); ax.set_ylabel("cycles per op")
ax.set_title("cycles per op vs T"); ax.grid(True, alpha=0.3)

# IPC
ax = axes[0,1]
ipc = [med(T, "instructions")/med(T, "cycles") if med(T, "cycles") else 0 for T in Ts]
ax.plot(Ts, ipc, marker="o", linewidth=2, color="#2ca02c")
ax.set_xscale("log", base=2); ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T"); ax.set_ylabel("IPC (instructions per cycle)")
ax.set_title("IPC vs T"); ax.grid(True, alpha=0.3)

# LLC miss %
ax = axes[1,0]
miss_pct = [med(T, "llc_load_misses")/med(T, "llc_loads")*100 if med(T, "llc_loads") else 0 for T in Ts]
ax.plot(Ts, miss_pct, marker="o", linewidth=2, color="#d62728")
ax.set_xscale("log", base=2); ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T"); ax.set_ylabel("LLC miss %"); ax.set_ylim(0, 100)
ax.set_title("LLC miss rate vs T"); ax.grid(True, alpha=0.3)

# thpt
ax = axes[1,1]
thpt = [med(T, "thpt_Mops") for T in Ts]
ax.plot(Ts, thpt, marker="o", linewidth=2, color="#ff7f0e")
ax.set_xscale("log", base=2); ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T"); ax.set_ylabel("Throughput (Mops/s)")
ax.set_title("thpt vs T (probe-off)"); ax.grid(True, alpha=0.3)

plt.tight_layout()
out = os.path.join(OUT_DIR, "perfstat_T_summary.png")
plt.savefig(out, dpi=140); plt.close()
print(f"wrote {out}")

# Text summary
out = os.path.join(OUT_DIR, "perfstat_summary.txt")
with open(out, "w") as f:
    f.write("iter-16A perfstat T-sweep — median per T\n")
    f.write("=" * 90 + "\n")
    f.write(f"{'T':>3} {'thpt':>8} {'cycles/op':>12} {'IPC':>6} {'cache_miss/op':>14} {'LLC_miss%':>10} {'w_p50_us':>10}\n")
    for T in Ts:
        cyc = med(T, "cycles"); ins = med(T, "instructions"); cm = med(T, "cache_misses")
        ll = med(T, "llc_loads"); llm = med(T, "llc_load_misses")
        cyc_per_op = cyc/5e6
        ipc = (ins/cyc) if cyc else 0
        cm_per_op = cm/5e6
        llcmp = (llm/ll*100) if ll else 0
        f.write(f"{T:>3} {med(T,'thpt_Mops'):>8.3f} {cyc_per_op:>12.0f} {ipc:>6.2f} "
                f"{cm_per_op:>14.1f} {llcmp:>9.2f}% {med(T,'w_p50_us'):>10.1f}\n")
print(f"wrote {out}")
