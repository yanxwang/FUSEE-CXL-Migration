#!/usr/bin/env python3
"""iter-16A RN-study plot: thpt vs T per NOOP level."""
import csv, sys, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import statistics

if len(sys.argv) < 2:
    print("Usage: iter16A_rn_plot.py <grid.csv> [scenario=xhost_write]")
    sys.exit(1)

CSV = sys.argv[1]
SCEN = sys.argv[2] if len(sys.argv) > 2 else "xhost_write"
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

# data[level][T] = list of thpt across reps
data = defaultdict(lambda: defaultdict(list))
with open(CSV) as f:
    r = csv.DictReader(f)
    for row in r:
        if row["scenario"] != SCEN:
            continue
        L = int(row["noop_level"])
        T = int(row["T"])
        try:
            t = float(row["thpt_Mops"])
        except ValueError:
            continue
        data[L][T].append(t)

levels = sorted(data.keys())
Ts = sorted({T for L in levels for T in data[L]})

LEVEL_LABEL = {
    0: "L0 full (= baseline)",
    1: "L1 skip invalidate broadcast",
    2: "L2 only CXL slot publish + ack",
    3: "L3 pure ack (ring ceiling)",
}
COLOR = {0: "#1f77b4", 1: "#ff7f0e", 2: "#2ca02c", 3: "#d62728"}

def make_plot(yscale, fname_suffix):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for L in levels:
        xs = sorted(data[L].keys())
        med = [statistics.median(data[L][T]) for T in xs]
        mn  = [min(data[L][T]) for T in xs]
        mx  = [max(data[L][T]) for T in xs]
        ax.plot(xs, med, marker="o", color=COLOR.get(L), label=LEVEL_LABEL.get(L, f"L{L}"))
        ax.fill_between(xs, mn, mx, color=COLOR.get(L), alpha=0.15)
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
    if yscale == "log":
        ax.set_yscale("log")
    ax.set_xlabel("T (workers/host)")
    ax.set_ylabel("Throughput per host (Mops/s)")
    ax.set_title(f"{SCEN} — Receiver NOOP study\n(median of 3 reps; shaded = min/max)")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="upper left", fontsize=8)
    out = os.path.join(OUT_DIR, f"{SCEN}_rn_plot_{fname_suffix}.png")
    plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
    print(f"wrote {out}")

make_plot("linear", "lin")
make_plot("log",    "log")

# Summary table
out = os.path.join(OUT_DIR, f"{SCEN}_rn_summary.txt")
with open(out, "w") as f:
    f.write(f"{SCEN} — RN study median thpt (Mops/s, per host)\n")
    f.write("=" * 72 + "\n")
    hdr = "T\\L  " + "  ".join(f"L{L}".rjust(8) for L in levels) + "    L3/L0   L2/L0   L1/L0\n"
    f.write(hdr)
    for T in Ts:
        meds = {L: statistics.median(data[L][T]) if T in data[L] else None for L in levels}
        row = f"{T:<4}"
        for L in levels:
            row += f"  {meds[L]:8.3f}" if meds[L] is not None else "  " + " " * 8
        l0 = meds.get(0)
        if l0:
            row += f"     {meds.get(3,0)/l0:5.2f}   {meds.get(2,0)/l0:5.2f}   {meds.get(1,0)/l0:5.2f}"
        f.write(row + "\n")
print(f"wrote {out}")
with open(out) as f:
    print(f.read())
