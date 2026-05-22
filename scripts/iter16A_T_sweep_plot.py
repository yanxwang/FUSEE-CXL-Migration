#!/usr/bin/env python3
"""iter-16A xhost write T-sweep: loglin plot + summary table PNG.

Usage:
  iter16A_T_sweep_plot.py <grid.csv>

Reads grid.csv (scenario,T,rep,thpt_Mops,...), produces:
  - xhost_write_T_plot_loglin.png  — line plot (log-y + lin-x)
  - xhost_write_T_summary_table.png — tabular thpt by T
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: iter16A_T_sweep_plot.py <grid.csv>")
    sys.exit(1)

CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

data = defaultdict(list)  # T -> list of thpt
lat_w_p50 = defaultdict(list)
lat_w_p99 = defaultdict(list)
with open(CSV) as f:
    r = csv.DictReader(f)
    for row in r:
        try:
            T = int(row["T"])
            thpt = float(row["thpt_Mops"])
            wp50 = float(row["w_p50_us"])
            wp99 = float(row["w_p99_us"])
        except (ValueError, KeyError):
            continue
        data[T].append(thpt)
        lat_w_p50[T].append(wp50)
        lat_w_p99[T].append(wp99)

Ts = sorted(data.keys())

# --- Plot ---
fig, ax = plt.subplots(figsize=(7, 4.5))
med = [statistics.median(data[T]) for T in Ts]
mn = [min(data[T]) for T in Ts]
mx = [max(data[T]) for T in Ts]
ax.plot(Ts, med, marker="o", linewidth=2, color="#1f77b4", label="thpt (Mops/s)")
ax.fill_between(Ts, mn, mx, color="#1f77b4", alpha=0.15)
ax.set_xscale("log", base=2)
ax.set_xticks(Ts)
ax.set_xticklabels([str(t) for t in Ts])
ax.set_xlabel("T (workers per host)")
ax.set_ylabel("Cluster throughput (Mops/s)")
ax.set_title(
    f"xhost_write T-sweep (V=1024, cache=10%, zipf-0.99, 3 reps)\n"
    f"median ± range (shaded)"
)
ax.grid(True, alpha=0.3, which="both")
ax.legend(loc="lower right")
plt_path = os.path.join(OUT_DIR, "xhost_write_T_plot_loglin.png")
plt.tight_layout(); plt.savefig(plt_path, dpi=140); plt.close()
print(f"wrote {plt_path}")

# --- Summary table ---
fig, ax = plt.subplots(figsize=(9, 0.4 + 0.35 * len(Ts)))
ax.axis("off")
rows = []
for T in Ts:
    med_t = statistics.median(data[T])
    p50 = statistics.median(lat_w_p50[T])
    p99 = statistics.median(lat_w_p99[T])
    rows.append([T, f"{med_t:.3f}", f"{p50:.1f}", f"{p99:.1f}",
                 f"{min(data[T]):.3f}", f"{max(data[T]):.3f}"])
cols = ["T", "thpt p50 (Mops/s)", "w_p50 (µs)", "w_p99 (µs)", "thpt min", "thpt max"]
tbl = ax.table(cellText=rows, colLabels=cols, loc="center", cellLoc="right")
tbl.auto_set_font_size(False); tbl.set_fontsize(10); tbl.scale(1, 1.3)
ax.set_title("xhost_write T-sweep summary (3 reps, median)", pad=12)
tbl_path = os.path.join(OUT_DIR, "xhost_write_T_summary_table.png")
plt.tight_layout(); plt.savefig(tbl_path, dpi=140); plt.close()
print(f"wrote {tbl_path}")

# Also a text summary
txt_path = os.path.join(OUT_DIR, "xhost_write_T_summary.txt")
with open(txt_path, "w") as f:
    f.write("xhost_write T-sweep — median thpt (Mops/s, cluster), 3 reps\n")
    f.write("=" * 60 + "\n")
    f.write(f"{'T':>4s}  {'thpt p50':>10s}  {'thpt min':>10s}  {'thpt max':>10s}  {'w_p50_us':>10s}  {'w_p99_us':>10s}\n")
    for T in Ts:
        med_t = statistics.median(data[T])
        f.write(f"{T:>4d}  {med_t:>10.3f}  {min(data[T]):>10.3f}  {max(data[T]):>10.3f}"
                f"  {statistics.median(lat_w_p50[T]):>10.1f}  {statistics.median(lat_w_p99[T]):>10.1f}\n")
print(f"wrote {txt_path}")
