#!/usr/bin/env python3
"""Exp 2 perf-lock-contention analysis plots.

Inputs:
  zipf_perf_stat.txt, uniform_perf_stat.txt — perf stat dumps
  zipf_sanity.txt, uniform_sanity.txt        — measured trans_agg_thpt

Outputs:
  exp2_perf_stat_bars.png — 4-panel bar chart (cycles, IPC, LLC miss%, thpt)
                            zipf vs uniform side-by-side
  exp2_perf_stat_table.png — clean matplotlib table summary
"""
import re, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

EXP2 = "/home/yanwang/FUSEE/docs/iter17A_exp2_perf_lock_20260523_030306"

def parse_perf_stat(path):
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            m = re.match(r'^([\d,]+)\s+(\S+)', line)
            if m:
                val_str = m.group(1).replace(",", "")
                name = m.group(2)
                d[name] = float(val_str)
            # IPC line: "12409179841      instructions          #    0.06  insn per cycle"
            m2 = re.search(r'#\s+([\d.]+)\s+(insn per cycle|of all cache refs|of all LL-cache accesses)', line)
            if m2:
                # store the # field with key based on context
                pass
    return d

zipf = parse_perf_stat(os.path.join(EXP2, "zipf_perf_stat.txt"))
uniform = parse_perf_stat(os.path.join(EXP2, "uniform_perf_stat.txt"))

# Derived metrics
def derive(d):
    ipc = d["instructions"] / d["cycles"]
    cache_miss_pct = 100.0 * d["cache-misses"] / d["cache-references"]
    llc_miss_pct = 100.0 * d["LLC-load-misses"] / d["LLC-loads"]
    return ipc, cache_miss_pct, llc_miss_pct

z_ipc, z_cm, z_llc = derive(zipf)
u_ipc, u_cm, u_llc = derive(uniform)

# Read sanity throughput
def read_thpt(path):
    with open(path) as f:
        for line in f:
            m = re.match(r'thpt=(\d+)', line.strip())
            if m: return int(m.group(1)) / 1e6  # Mops/s
    return 0
z_thpt = read_thpt(os.path.join(EXP2, "zipf_sanity.txt"))
u_thpt = read_thpt(os.path.join(EXP2, "uniform_sanity.txt"))

print(f"ZIPF   : cyc={zipf['cycles']/1e9:.1f}G  ins={zipf['instructions']/1e9:.2f}G  IPC={z_ipc:.3f}  cache_miss={z_cm:.1f}%  LLC_miss={z_llc:.1f}%  thpt={z_thpt:.2f} Mops")
print(f"UNIFORM: cyc={uniform['cycles']/1e9:.1f}G  ins={uniform['instructions']/1e9:.2f}G  IPC={u_ipc:.3f}  cache_miss={u_cm:.1f}%  LLC_miss={u_llc:.1f}%  thpt={u_thpt:.2f} Mops")

# ----- Plot: 4 sub-panels -----
fig, axes = plt.subplots(1, 4, figsize=(17, 4.8))
dists = ["zipf-0.99", "uniform"]
colors = ["#d62728", "#1f77b4"]

# Panel A: Total cycles (3s window) — work expended
ax = axes[0]
vals = [zipf["cycles"]/1e9, uniform["cycles"]/1e9]
bars = ax.bar(dists, vals, color=colors, edgecolor="black", linewidth=0.5)
for b, v in zip(bars, vals): ax.text(b.get_x()+b.get_width()/2, v+1, f"{v:.1f}G", ha="center", fontsize=10, fontweight="bold")
ax.set_ylabel("Total cycles across 22 receivers (G)", fontsize=10)
ax.set_title("(a) Total cycles (3s perf window)\n— similar work expended", fontsize=10.5)
ax.set_ylim(0, max(vals)*1.15)

# Panel B: IPC
ax = axes[1]
vals = [z_ipc, u_ipc]
bars = ax.bar(dists, vals, color=colors, edgecolor="black", linewidth=0.5)
for b, v in zip(bars, vals): ax.text(b.get_x()+b.get_width()/2, v+0.005, f"{v:.3f}", ha="center", fontsize=10, fontweight="bold")
ax.set_ylabel("IPC (instructions per cycle)", fontsize=10)
ax.set_title("(b) IPC — both ≈0.06 = memory-bound\n(spin-lock would give IPC ≥ 0.3)", fontsize=10.5)
ax.set_ylim(0, max(vals)*1.45)

# Panel C: LLC miss%
ax = axes[2]
vals_cm = [z_cm, u_cm]
vals_llc = [z_llc, u_llc]
x = np.arange(2)
width = 0.32
b1 = ax.bar(x - width/2, vals_cm, width, label="L1+L2 miss %", color="#ff9933", edgecolor="black", linewidth=0.5)
b2 = ax.bar(x + width/2, vals_llc, width, label="LLC miss %", color="#bb3300", edgecolor="black", linewidth=0.5)
for bars, vals in [(b1, vals_cm), (b2, vals_llc)]:
    for b, v in zip(bars, vals):
        ax.text(b.get_x()+b.get_width()/2, v+1, f"{v:.1f}%", ha="center", fontsize=9, fontweight="bold")
ax.set_xticks(x); ax.set_xticklabels(dists)
ax.set_ylabel("Miss ratio (%)", fontsize=10)
ax.set_ylim(0, max(vals_cm + vals_llc)*1.18)
ax.set_title("(c) Cache miss ratios\nzipf +24% LLC misses = hot-key coherence ping-pong", fontsize=10.5)
ax.legend(loc="upper right", fontsize=9)

# Panel D: throughput sanity
ax = axes[3]
vals = [z_thpt, u_thpt]
bars = ax.bar(dists, vals, color=colors, edgecolor="black", linewidth=0.5)
for b, v in zip(bars, vals): ax.text(b.get_x()+b.get_width()/2, v+0.1, f"{v:.2f}", ha="center", fontsize=10, fontweight="bold")
ax.set_ylabel("Cluster throughput (Mops/s)", fontsize=10)
ax.set_title("(d) Measured cluster thpt during perf window\n(T=32 N=4 worker_id, 1 rep)", fontsize=10.5)
ax.set_ylim(0, max(vals)*1.25)

fig.suptitle("iter-17A Exp 2: perf sampling on 22 receiver threads — T=32 N=4 worker_id, zipf-0.99 vs uniform xhost_write\n"
             "Finding: bottleneck is CXL-memory-bound (IPC≈0.06, LLC miss 73 % zipf vs 59 % uniform), NOT pthread_spin_lock contention",
             fontsize=12, y=1.01)
plt.tight_layout(rect=[0, 0, 1, 0.95])
out1 = os.path.join(EXP2, "exp2_perf_stat_bars.png")
plt.savefig(out1, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out1}")

# ----- Plot: clean comparison table -----
fig, ax = plt.subplots(figsize=(11, 3.4))
ax.axis("off")
headers = ["Metric", "zipf-0.99", "uniform", "delta", "Interpretation"]
rows = [
    ["Total cycles (3s, 22 recvs)", f"{zipf['cycles']/1e9:.1f} G", f"{uniform['cycles']/1e9:.1f} G",
     f"{(zipf['cycles']-uniform['cycles'])/uniform['cycles']*100:+.1f}%", "Similar — both fully running"],
    ["Total instructions (3s)",      f"{zipf['instructions']/1e9:.2f} G", f"{uniform['instructions']/1e9:.2f} G",
     f"{(zipf['instructions']-uniform['instructions'])/uniform['instructions']*100:+.1f}%", "Effectively identical work"],
    ["IPC",                          f"{z_ipc:.3f}", f"{u_ipc:.3f}",
     f"{(z_ipc-u_ipc)/u_ipc*100:+.1f}%", "≈0.06 both → memory-bound (lock spin ≥ 0.3)"],
    ["L1+L2 cache miss %",           f"{z_cm:.1f}%", f"{u_cm:.1f}%",
     f"{z_cm-u_cm:+.1f}pp", "zipf shoots higher = more capacity miss"],
    ["LLC load miss %",              f"{z_llc:.1f}%", f"{u_llc:.1f}%",
     f"{z_llc-u_llc:+.1f}pp", "**zipf +14pp → coherence ping-pong evidence**"],
    ["Cluster throughput (Mops/s)",  f"{z_thpt:.2f}", f"{u_thpt:.2f}",
     f"{(z_thpt-u_thpt)/u_thpt*100:+.1f}%", "Uniform marginally faster (1 rep)"],
]
tbl = ax.table(cellText=rows, colLabels=headers, loc="center", cellLoc="center", colLoc="center")
tbl.auto_set_font_size(False)
tbl.set_fontsize(10)
tbl.scale(1.0, 1.7)
for j in range(len(headers)):
    tbl[(0, j)].set_facecolor("#cfcfcf")
    tbl[(0, j)].set_text_props(weight="bold")
for i in range(1, len(rows)+1):
    tbl[(i, 0)].set_text_props(weight="bold", ha="left")
    tbl[(i, 0)].set_facecolor("#f0f0f0")
    tbl[(i, len(headers)-1)].set_text_props(ha="left")
    # Highlight the LLC row
    if "LLC load miss" in rows[i-1][0]:
        for j in range(len(headers)):
            tbl[(i, j)].set_facecolor("#fff2cc")
col_widths = [0.22, 0.12, 0.12, 0.10, 0.44]
for i in range(len(rows)+1):
    for j, w in enumerate(col_widths): tbl[(i, j)].set_width(w)

ax.set_title("iter-17A Exp 2: perf-stat on receivers (T=32 N=4 worker_id, 3 s sample)\n"
             "Original claim 'T=32 zipf = pthread_spin_lock contention' refined: actual bottleneck is CXL coherence ping-pong on hot-bucket lines",
             fontsize=11.5, pad=10, weight="bold")
plt.tight_layout()
out2 = os.path.join(EXP2, "exp2_perf_stat_table.png")
plt.savefig(out2, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out2}")
