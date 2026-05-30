#!/usr/bin/env python3
"""iter-18A xhost_read combined T-sweep decomp — mirror of iter-16A
Stage5_Gap_StageR_combined.png. For read: Stage4 (ack_wait) replaces
Stage5; StageR = R1+R2+R3.

Inputs (hardcoded from existing files):
  - per-T decomp CSVs: docs/iter18A_phase2_T_sweep_20260523_044157/decomp_T{T}.csv
  - gap rate: docs/iter18A_phase2_T_sweep_20260523_044157/decomp.log (parsed)
"""
import csv, os, re, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DATA = "docs/iter18A_phase2_T_sweep_20260523_044157"
OUT_DIR = DATA

Ts = [1, 2, 4, 8, 16, 32, 64]

def med(T, fld):
    rows = []
    fp = os.path.join(DATA, f"decomp_T{T}.csv")
    with open(fp) as f:
        for r in csv.DictReader(f):
            try: rows.append(int(r[fld]))
            except: pass
    return statistics.median(rows)

stage1 = [med(T, "Stage1") for T in Ts]
stage2 = [med(T, "Stage2") for T in Ts]
stage3 = [med(T, "Stage3") for T in Ts]
stage4 = [med(T, "Stage4") for T in Ts]   # ack_wait (read's dominant stage)
stage5 = [med(T, "Stage5") for T in Ts]   # cleanup_validate
stage6 = [med(T, "Stage6") for T in Ts]   # value_recv
stagew = [med(T, "StageW") for T in Ts]
r1     = [med(T, "StageR1") for T in Ts]
r2     = [med(T, "StageR2") for T in Ts]
r3     = [med(T, "StageR3") for T in Ts]
stager = [med(T, "StageR") for T in Ts]
queueing = [s - r for s, r in zip(stage4, stager)]

# Gap rate per T parsed from decomp.log
gap_rate = []
with open(os.path.join(DATA, "decomp.log")) as f:
    txt = f.read()
matches = re.findall(r"gap encounter rate = ([\d.]+)% of receiver ops", txt)
gap_rate = [float(m) for m in matches[:len(Ts)]]
assert len(gap_rate) == len(Ts), f"gap_rate len {len(gap_rate)} vs Ts {len(Ts)}"

# Throughput (cluster Mops/s, probe-off median)
import csv as _csv
thpt = {}
with open(os.path.join(DATA, "grid.csv")) as f:
    for r in _csv.DictReader(f):
        if r["build"] == "off":
            t = int(r["T"])
            thpt.setdefault(t, []).append(int(r["thpt"]))
thpt_med = [statistics.median(thpt[T])/1e6 for T in Ts]

# R1 (ring_drain) component model: at T=1 it's inflated by gap-wait.
# Similar to iter-16A's Stage6 model: R1 = R1_base + gap_rate × R1_gap_wait
# Pick R1_base from T=64 (gap ~0) and R1_gap_wait calibrated from T=1
R1_BASE = r1[-1]                                # T=64 ~7022 ns
R1_GAP = (r1[0] - R1_BASE) / (gap_rate[0]/100)  # T=1 inflation factor
r1_base_part = [R1_BASE if r >= R1_BASE else r for r in r1]
r1_gap_part = [r - rb for r, rb in zip(r1, r1_base_part)]

xs = [str(T) for T in Ts]

fig = plt.figure(figsize=(20, 12))
gs = fig.add_gridspec(3, 2, width_ratios=[1.8, 1.0], hspace=0.55, wspace=0.05)

def make_table(ax, headers, rows, title, col_widths=None):
    ax.axis("off")
    tbl = ax.table(cellText=rows, colLabels=headers, loc="center",
                   cellLoc="center", colWidths=col_widths)
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(9)
    tbl.scale(1.0, 1.6)
    for j in range(len(headers)):
        tbl[(0, j)].set_facecolor("#dddddd")
        tbl[(0, j)].set_text_props(weight="bold")
    ax.set_title(title, fontsize=10, pad=8, weight="bold")

# Panel 1: Stage 4 (ack_wait) composition
ax = fig.add_subplot(gs[0, 0])
ax.bar(xs, stager, label="StageR (= XRR1+XRR2+XRR3, receiver pipeline)", color="#2ca02c")
ax.bar(xs, queueing, bottom=stager,
       label="Stage4 − StageR (= worker spin + CXL prop + queueing)", color="#d62728")
ax.set_ylabel("Latency (ns), log scale")
ax.set_yscale("log")
ax.set_ylim(min(stager)*0.6, max(stage4)*4)
ax.set_title("Panel 1 — Stage 4 / XRS4 (worker ack_wait) composition",
             fontsize=11, pad=12)
ax.grid(True, alpha=0.3, axis="y", which="both")
ax.legend(loc="upper left", fontsize=9)
for i, (sr, s4) in enumerate(zip(stager, stage4)):
    ratio = sr / s4 * 100
    ax.text(i, s4 * 0.92, f"StageR={ratio:.1f}%", ha="center", va="top",
            fontsize=8, color="white", weight="bold")

# Table 1
ax = fig.add_subplot(gs[0, 1])
rows = []
for T, s4, sr, q in zip(Ts, stage4, stager, queueing):
    pct = sr/s4*100
    rows.append([str(T), f"{s4:.0f}", f"{sr:.0f}", f"{q:.0f}", f"{pct:.1f}%"])
make_table(ax, ["T", "Stage4", "StageR", "S4−SR", "SR/S4"], rows,
           "Stage 4 (XRS4) decomposition (ns, p50)",
           col_widths=[0.10, 0.22, 0.22, 0.22, 0.18])

# Panel 2: Gap rate
ax = fig.add_subplot(gs[1, 0])
ax.plot(xs, gap_rate, marker="o", linewidth=2, color="#ff7f0e",
        label="Gap rate (XRR1Z / receiver ops)")
ax.set_ylabel("Gap rate (%)")
ax.set_ylim(-5, 115)
ax.axhline(50, color="gray", linestyle="--", alpha=0.4)
ax.set_title("Panel 2 — Receiver gap rate (XRR1Z, receiver polled but worker hadn't published yet)",
             fontsize=11, pad=12)
ax.grid(True, alpha=0.3)
ax.legend(loc="center right", fontsize=9)
for i, g in enumerate(gap_rate):
    if g > 70:
        ax.text(i, g-6, f"{g:.1f}%", ha="center", va="top", fontsize=8)
    else:
        ax.text(i, g+4, f"{g:.1f}%", ha="center", va="bottom", fontsize=8)

# Table 2
ax = fig.add_subplot(gs[1, 1])
rows = [[str(T), f"{g:.2f}%"] for T, g in zip(Ts, gap_rate)]
make_table(ax, ["T", "Gap rate"], rows,
           "Gap rate (% of receiver ops)",
           col_widths=[0.20, 0.30])

# Panel 3: StageR composition (R1 split into base + gap_inflation, plus R2, R3)
ax = fig.add_subplot(gs[2, 0])
ax.bar(xs, r1_base_part,
       label=f"XRR1 base (= ring_drain ~{R1_BASE:.0f} ns)", color="#1f77b4")
ax.bar(xs, r1_gap_part, bottom=r1_base_part,
       label="XRR1 gap-wait inflation", color="#ff7f0e")
bot2 = [a+b for a,b in zip(r1_base_part, r1_gap_part)]
ax.bar(xs, r2, bottom=bot2,
       label="XRR2 (handler: bucket+dir+staging publish)", color="#2ca02c")
bot3 = [b+s for b, s in zip(bot2, r2)]
ax.bar(xs, r3, bottom=bot3,
       label="XRR3 (ack_publish)", color="#9467bd")
ax.set_xlabel("T (workers per host)")
ax.set_ylabel("Latency (ns)")
ax.set_ylim(0, max(stager) * 1.25)
ax.set_title("Panel 3 — StageR composition: XRR2 (~1.5 µs) flat, variation is XRR1 gap-wait inflating",
             fontsize=11, pad=12)
ax.grid(True, alpha=0.3, axis="y")
ax.legend(loc="upper right", fontsize=9)
for i, sr in enumerate(stager):
    ax.text(i, sr + max(stager)*0.02, f"{sr:.0f}",
            ha="center", va="bottom", fontsize=8)

# Table 3
ax = fig.add_subplot(gs[2, 1])
rows = []
for T, sr, base, gappart, r2v, r3v in zip(Ts, stager, r1_base_part,
                                           r1_gap_part, r2, r3):
    rows.append([str(T), f"{sr:.0f}", f"{base:.0f}", f"{gappart:.0f}",
                 f"{r2v:.0f}", f"{r3v:.0f}"])
make_table(ax, ["T", "StageR", "R1_base", "R1_gap", "R2", "R3"], rows,
           "StageR decomposition (ns, p50)",
           col_widths=[0.10, 0.16, 0.18, 0.16, 0.14, 0.12])

out = os.path.join(OUT_DIR, "Stage4_Gap_StageR_combined.png")
plt.savefig(out, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out}")
