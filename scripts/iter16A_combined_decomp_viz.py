#!/usr/bin/env python3
"""iter-16A combined T-sweep decomp viz — 3 panels share x-axis T:
  Panel 1: Stage 5 composition (StageR + queueing overhead)
  Panel 2: Gap rate %
  Panel 3: StageR composition (Stage6 split into G3-base + gap-wait, plus Stage7, Stage8)

Shows the full causal chain:
  - At low T: gap rate high → Stage 6 inflated by gap-wait → StageR looks "slow"
  - At high T: gap rate ~0 → Stage 6 = G3 only → StageR drops
  - Stage 7 (real receiver work) is flat across all T
  - Stage 5 - StageR = worker queueing, grows linearly with T
"""
import csv, sys, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = sys.argv[1]
import os
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

by_T = defaultdict(lambda: defaultdict(list))
with open(CSV) as f:
    for r in csv.DictReader(f):
        T = int(r["T"])
        for k, v in r.items():
            if k in ("T","rep"): continue
            try: by_T[T][k].append(float(v))
            except: pass

Ts = sorted(by_T.keys())
med = lambda T, k: statistics.median(by_T[T].get(k, [0]))

stage5 = [med(T, "stage5_p50_ns") for T in Ts]
stage6 = [med(T, "stage6_p50_ns") for T in Ts]
stage7 = [med(T, "stage7_p50_ns") for T in Ts]
stage8 = [med(T, "stage8_p50_ns") for T in Ts]
stager = [med(T, "stager_p50_ns") for T in Ts]
queueing = [s5 - sr for s5, sr in zip(stage5, stager)]
gap_rate = [med(T, "gap_rate_pct") for T in Ts]
thpt = [med(T, "thpt_Mops") for T in Ts]

# Model: Stage 6 = G3 + gap_rate × G_gap; from T=64: G3 ≈ 1100, T=1: G_gap ≈ 1880
G3_BASE = 1100  # ns
G_GAP = 1880    # ns
stage6_gap_part = [(g/100.0) * G_GAP for g in gap_rate]
stage6_base_part = [s6 - sg for s6, sg in zip(stage6, stage6_gap_part)]

xs = [str(t) for t in Ts]

# 3 rows × 2 cols: left wide (charts), right wider (tables) — bump width + table-col widths
fig = plt.figure(figsize=(20, 12))
gs = fig.add_gridspec(3, 2, width_ratios=[1.8, 1.0], hspace=0.55, wspace=0.05)

def make_table(ax, headers, rows, title, col_widths=None):
    ax.axis("off")
    tbl = ax.table(cellText=rows, colLabels=headers, loc="center", cellLoc="center",
                   colWidths=col_widths)
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(9)
    tbl.scale(1.0, 1.6)
    for j in range(len(headers)):
        tbl[(0, j)].set_facecolor("#dddddd")
        tbl[(0, j)].set_text_props(weight="bold")
    ax.set_title(title, fontsize=10, pad=8, weight="bold")

# Panel 1 (chart): Stage 5 composition stacked
ax = fig.add_subplot(gs[0, 0])
ax.bar(xs, stager, label="StageR (= Stage6+7+8, receiver pipeline)", color="#2ca02c")
ax.bar(xs, queueing, bottom=stager,
       label="Stage5 − StageR (= worker spin + CXL prop + queueing)", color="#d62728")
ax.set_ylabel("Latency (ns), log scale")
ax.set_yscale("log")
# Expand top room so annotations don't collide with title
ax.set_ylim(min(stager)*0.6, max(stage5)*4)
ax.set_title("Panel 1 — Stage 5 (worker ack_wait) composition", fontsize=11, pad=12)
ax.grid(True, alpha=0.3, axis="y", which="both")
ax.legend(loc="upper left", fontsize=9)
# Put StageR% label INSIDE the top of each stacked bar (white text)
for i, (sr, s5) in enumerate(zip(stager, stage5)):
    ratio = sr / s5 * 100
    ax.text(i, s5 * 0.92, f"StageR={ratio:.1f}%", ha="center", va="top",
            fontsize=8, color="white", weight="bold")

# Table 1: Stage 5 decomposition — shorter headers + col widths
ax = fig.add_subplot(gs[0, 1])
rows = []
for T, s5, sr, q in zip(Ts, stage5, stager, queueing):
    pct = sr/s5*100
    rows.append([str(T), f"{s5:.0f}", f"{sr:.0f}", f"{q:.0f}", f"{pct:.1f}%"])
make_table(ax, ["T", "Stage5", "StageR", "S5−SR", "SR/S5"],
           rows, "Stage 5 decomposition (ns, p50)",
           col_widths=[0.10, 0.20, 0.20, 0.20, 0.18])

# Panel 2 (chart): Gap rate
ax = fig.add_subplot(gs[1, 0])
ax.plot(xs, gap_rate, marker="o", linewidth=2, color="#ff7f0e", label="Gap rate (XWR6Z / total)")
ax.set_ylabel("Gap rate (%)")
# Expand top to leave room for highest annotation (~99% + 3 = 102, plus title pad)
ax.set_ylim(-5, 115)
ax.axhline(50, color="gray", linestyle="--", alpha=0.4)
ax.set_title("Panel 2 — Receiver gap rate (receiver picked up but worker hadn't published yet)",
             fontsize=11, pad=12)
ax.grid(True, alpha=0.3)
ax.legend(loc="center right", fontsize=9)
for i, g in enumerate(gap_rate):
    # Place annotation BELOW the point when high so it doesn't collide with title
    if g > 70:
        ax.text(i, g-6, f"{g:.1f}%", ha="center", va="top", fontsize=8)
    else:
        ax.text(i, g+4, f"{g:.1f}%", ha="center", va="bottom", fontsize=8)

# Table 2: Gap rate
ax = fig.add_subplot(gs[1, 1])
rows = []
for T, g in zip(Ts, gap_rate):
    rows.append([str(T), f"{g:.2f}%"])
make_table(ax, ["T", "Gap rate"], rows, "Gap rate (% of receiver ops)",
           col_widths=[0.20, 0.30])

# Panel 3 (chart): StageR composition with Stage 6 split
ax = fig.add_subplot(gs[2, 0])
ax.bar(xs, stage6_base_part, label="Stage6 base (= G3, single CXL read+fence)", color="#1f77b4")
ax.bar(xs, stage6_gap_part, bottom=stage6_base_part,
       label="Stage6 gap-wait (= gap_rate × G_gap)", color="#ff7f0e")
bot2 = [b+g for b, g in zip(stage6_base_part, stage6_gap_part)]
ax.bar(xs, stage7, bottom=bot2, label="Stage7 (rcv_work)", color="#2ca02c")
bot3 = [b+s for b, s in zip(bot2, stage7)]
ax.bar(xs, stage8, bottom=bot3, label="Stage8 (ack_publish)", color="#9467bd")
ax.set_xlabel("T (workers per host)")
ax.set_ylabel("Latency (ns)")
# Expand top to leave room for annotation
ax.set_ylim(0, max(stager) * 1.25)
ax.set_title("Panel 3 — StageR composition: receiver work flat (Stage7), variation is gap-wait inflating Stage 6",
             fontsize=11, pad=12)
ax.grid(True, alpha=0.3, axis="y")
ax.legend(loc="upper right", fontsize=9)
# Move totals annotation just above bars, scaled small so it fits under title
for i, sr in enumerate(stager):
    ax.text(i, sr + max(stager)*0.02, f"{sr:.0f}", ha="center", va="bottom", fontsize=8)
ax.text(0.99, 0.55, f"model: Stage6 ≈ G3({G3_BASE}ns) + gap_rate × G_gap({G_GAP}ns)",
        transform=ax.transAxes, ha="right", fontsize=8, style="italic",
        bbox=dict(facecolor='white', alpha=0.7, edgecolor='gray'))

# Table 3: StageR decomposition — give wider columns for clarity
ax = fig.add_subplot(gs[2, 1])
rows = []
for T, sr, s6, s7, s8, s6b, s6g in zip(Ts, stager, stage6, stage7, stage8,
                                        stage6_base_part, stage6_gap_part):
    rows.append([str(T), f"{sr:.0f}", f"{s6b:.0f}", f"{s6g:.0f}",
                 f"{s7:.0f}", f"{s8:.0f}"])
make_table(ax, ["T", "StageR", "S6_base", "S6_gap", "S7", "S8"],
           rows, "StageR decomposition (ns, p50)",
           col_widths=[0.10, 0.16, 0.18, 0.18, 0.14, 0.14])

# Skip tight_layout (incompatible with mpl Tables); rely on gridspec hspace/wspace.
out = os.path.join(OUT_DIR, "Stage5_Gap_StageR_combined.png")
plt.savefig(out, dpi=140, bbox_inches="tight"); plt.close()
print(f"wrote {out}")
