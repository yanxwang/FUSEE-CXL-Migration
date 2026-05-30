#!/usr/bin/env python3
"""Side-by-side comparison: iter-16A xhost_write Stage5 vs iter-18A
xhost_read Stage4 (both are the ack_wait stage).

Three panels mirror the Stage5_Gap_StageR_combined structure but
two columns: left=write, right=read.
"""
import csv, os, re, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.expanduser("~/FUSEE")

# Write data
WRITE_CSV = f"{ROOT}/docs/iter16A_xhost_decomp_sweep_20260521_075204/aggregate.csv"
write_by_T = {}
with open(WRITE_CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"])
        write_by_T.setdefault(T, []).append(r)

def w_med(T, k):
    return statistics.median([float(r[k]) for r in write_by_T[T]])

Ts = sorted(write_by_T.keys())
w_s5 = [w_med(T, "stage5_p50_ns") for T in Ts]
w_sr = [w_med(T, "stager_p50_ns") for T in Ts]
w_gap = [w_med(T, "gap_rate_pct") for T in Ts]
w_thpt = [w_med(T, "thpt_Mops") for T in Ts]

# Read data
READ_DIR = f"{ROOT}/docs/iter18A_phase2_T_sweep_20260523_044157"

def r_med(T, fld):
    rows = []
    with open(f"{READ_DIR}/decomp_T{T}.csv") as f:
        for r in csv.DictReader(f):
            try: rows.append(int(r[fld]))
            except: pass
    return statistics.median(rows)

assert sorted(Ts) == sorted([1,2,4,8,16,32,64])
r_s4 = [r_med(T, "Stage4") for T in Ts]
r_sr = [r_med(T, "StageR") for T in Ts]

with open(f"{READ_DIR}/decomp.log") as f:
    txt = f.read()
r_gap = [float(m) for m in
         re.findall(r"gap encounter rate = ([\d.]+)%", txt)[:7]]

with open(f"{READ_DIR}/grid.csv") as f:
    thpt_acc = {}
    for r in csv.DictReader(f):
        if r["build"] == "off":
            T = int(r["T"])
            thpt_acc.setdefault(T, []).append(int(r["thpt"]))
r_thpt = [statistics.median(thpt_acc[T])/1e6 for T in Ts]

w_q = [s5-sr for s5,sr in zip(w_s5, w_sr)]
r_q = [s4-sr for s4,sr in zip(r_s4, r_sr)]
xs = [str(T) for T in Ts]

fig, axes = plt.subplots(3, 2, figsize=(18, 13))
fig.suptitle("xhost_write Stage5 (XWS5) vs xhost_read Stage4 (XRS4) — "
             "single-receiver T-sweep decomp comparison",
             fontsize=14, weight="bold", y=1.0)

# Row 1: Stage composition (worker side ack_wait stage)
for col, (label, sX, sr, q, color_s, title) in enumerate([
    ("xhost_write XWS5", w_s5, w_sr, w_q, "#d62728",
     "Stage 5 (XWS5) composition"),
    ("xhost_read XRS4",  r_s4, r_sr, r_q, "#d62728",
     "Stage 4 (XRS4) composition")]):
    ax = axes[0][col]
    ax.bar(xs, sr, color="#2ca02c", label="StageR")
    ax.bar(xs, q, bottom=sr, color=color_s,
           label=f"{label} − StageR (queueing + CXL prop)")
    ax.set_yscale("log")
    ax.set_ylim(min(sr)*0.5, max(sX)*5)
    ax.set_title(title, fontsize=11, weight="bold")
    ax.set_ylabel("ns (log)")
    ax.grid(True, alpha=0.3, axis="y", which="both")
    ax.legend(loc="upper left", fontsize=9)
    for i, (a, b) in enumerate(zip(sr, sX)):
        ax.text(i, b*0.92, f"{a/b*100:.1f}%", ha="center", va="top",
                fontsize=8, color="white", weight="bold")

# Row 2: Gap rate
for col, (gap, title) in enumerate([
    (w_gap, "Write gap rate (XWR6Z)"),
    (r_gap, "Read gap rate (XRR1Z)")]):
    ax = axes[1][col]
    ax.plot(xs, gap, marker="o", linewidth=2, color="#ff7f0e",
            label="Gap %")
    ax.axhline(50, color="gray", linestyle="--", alpha=0.4)
    ax.set_ylim(-5, 115)
    ax.set_title(title, fontsize=11, weight="bold")
    ax.set_ylabel("Gap rate (%)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="center right", fontsize=9)
    for i, g in enumerate(gap):
        if g > 70:
            ax.text(i, g-6, f"{g:.1f}%", ha="center", va="top", fontsize=8)
        else:
            ax.text(i, g+4, f"{g:.1f}%", ha="center", va="bottom", fontsize=8)

# Row 3: SR/SX ratio + throughput on twin-axis
for col, (ratio, thpt, title, color) in enumerate([
    ([sr/s*100 for sr,s in zip(w_sr, w_s5)], w_thpt,
     "Write SR/S5 ratio + thpt", "#1f77b4"),
    ([sr/s*100 for sr,s in zip(r_sr, r_s4)], r_thpt,
     "Read SR/S4 ratio + thpt", "#1f77b4")]):
    ax = axes[2][col]
    ax.plot(xs, ratio, marker="s", linewidth=2, color=color,
            label="SR/S(4|5) ratio (%)")
    ax.set_ylim(0, 90)
    ax.set_ylabel("SR / ack_wait (%)", color=color)
    ax.tick_params(axis='y', labelcolor=color)
    ax.set_xlabel("T (workers per host)")
    ax.set_title(title, fontsize=11, weight="bold")
    ax.grid(True, alpha=0.3)
    # twin axis for thpt
    ax2 = ax.twinx()
    ax2.plot(xs, thpt, marker="^", linewidth=2, color="#d62728",
             label="Throughput (Mops/s cluster)")
    ax2.set_ylabel("thpt (Mops/s)", color="#d62728")
    ax2.tick_params(axis='y', labelcolor="#d62728")
    for i, (r, t) in enumerate(zip(ratio, thpt)):
        ax.text(i, r+2, f"{r:.1f}%", ha="center", va="bottom",
                fontsize=8, color=color)
    # Combined legend
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1+h2, l1+l2, loc="center right", fontsize=9)

plt.tight_layout(rect=[0, 0, 1, 0.97])
out = f"{ROOT}/docs/iter18A_phase2_T_sweep_20260523_044157/Compare_write_vs_read_Stage5_Stage4.png"
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")

# Also print the data table for the response
print()
print(f"{'T':<6} | WRITE: S5         StageR    SR/S5    gap%    thpt | READ: S4         StageR    SR/S4    gap%    thpt")
print("-"*130)
for i, T in enumerate(Ts):
    ws5, wsr, wgp, wth = w_s5[i], w_sr[i], w_gap[i], w_thpt[i]
    rs4, rsr, rgp, rth = r_s4[i], r_sr[i], r_gap[i], r_thpt[i]
    wr = wsr/ws5*100
    rr = rsr/rs4*100
    print(f"{T:<6} | {ws5:>8.0f}    {wsr:>6.0f}   {wr:6.1f}%  {wgp:6.2f}%  {wth:5.3f} | {rs4:>8.0f}    {rsr:>6.0f}   {rr:6.1f}%  {rgp:6.2f}%  {rth:5.3f}")
