#!/usr/bin/env python3
"""Plot all Bell cluster experiments."""
import os, csv, numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = HERE

def pct(a, p): return int(np.percentile(a, p))

# ====== Exp 1: Latency CDF ======
ops = [("INSERT","insert_lat-3rp.txt","#1f77b4"),
       ("SEARCH","search_lat-3rp.txt","#2ca02c"),
       ("UPDATE","update_lat-3rp.txt","#ff7f0e"),
       ("DELETE","delete_lat-3rp.txt","#d62728")]
data = {}
for n, f, c in ops:
    a = np.loadtxt(os.path.join(HERE, "exp1_results", f), dtype=np.int64)
    data[n] = (a, c)

fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
for n, (a, c) in data.items():
    xs = np.sort(a); ys = np.arange(1, len(xs)+1) / len(xs)
    ax.plot(xs, ys, label=n, color=c, linewidth=1.8)
ax.set_xlabel("Latency (μs)"); ax.set_ylabel("CDF")
ax.set_xlim(0, 30); ax.grid(True, alpha=0.3); ax.legend(loc="lower right")
ax.set_title("Exp 1: single-op latency CDF — 3 MN + 1 CN  (b1/b2/b3 + b4)\nConnectX-4 @ FDR, num_replication=3")
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp1_cdf.png"))
print("wrote exp1_cdf.png")

# percentile summary bar
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
labels = list(data.keys()); x = np.arange(len(labels)); w = 0.22
p50 = [pct(data[n][0], 50)  for n in labels]
p95 = [pct(data[n][0], 95)  for n in labels]
p99 = [pct(data[n][0], 99)  for n in labels]
p999= [pct(data[n][0], 99.9)for n in labels]
ax.bar(x-1.5*w, p50, w, label="p50",  color="#1f77b4")
ax.bar(x-0.5*w, p95, w, label="p95",  color="#2ca02c")
ax.bar(x+0.5*w, p99, w, label="p99",  color="#ff7f0e")
ax.bar(x+1.5*w, p999,w, label="p99.9",color="#d62728")
for i, n in enumerate(labels):
    ax.text(i-1.5*w, p50[i]+0.2, f"{p50[i]}", ha="center", fontsize=8)
    ax.text(i+1.5*w, p999[i]+0.2, f"{p999[i]}", ha="center", fontsize=8)
ax.set_xticks(x); ax.set_xticklabels(labels)
ax.set_ylabel("Latency (μs)"); ax.grid(True, axis="y", alpha=0.3); ax.legend()
ax.set_title("Exp 1: tail-latency percentiles")
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp1_percentiles.png"))
print("wrote exp1_percentiles.png")

# ====== Exp 2: YCSB scaling ======
scaling = {"workloada": {}, "workloadc": {}}
with open(os.path.join(HERE, "exp2_scaling.csv")) as f:
    for r in csv.DictReader(f):
        if r["tpt_ops_per_sec"] == "0": continue
        scaling[r["workload"]][int(r["num_clients"])] = int(r["tpt_ops_per_sec"])

fig, (axa, axc) = plt.subplots(1, 2, figsize=(12, 4.5), dpi=130)
for ax, wl, title in [(axa, "workloada", "YCSB-A (50R/50U)"),
                      (axc, "workloadc", "YCSB-C (100R)")]:
    pts = sorted(scaling[wl].items()); xs = [p[0] for p in pts]; ys = [p[1]/1e6 for p in pts]
    ax.plot(xs, ys, "o-", color="#1f77b4", linewidth=2, markersize=7, label="FUSEE @ ConnectX-4 FDR")
    # ideal linear from N=1
    if xs[0] == 1:
        base = ys[0]
        ax.plot(xs, [x*base for x in xs], "--", color="gray", alpha=0.6, label="ideal linear")
    for xi, yi in zip(xs, ys):
        ax.annotate(f"{yi:.2f}", (xi, yi), xytext=(xi, yi*1.06),
                    ha="center", fontsize=8, color="#1f77b4")
    ax.set_xscale("log", base=2); ax.set_xticks(xs); ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlabel("Number of clients on CN (b4)")
    ax.set_ylabel("Throughput (M ops/s)")
    ax.set_title(title); ax.grid(True, alpha=0.3); ax.legend(loc="upper left", fontsize=9)
    # NUMA shaded
    ax.axvspan(7, max(xs)*1.05, alpha=0.08, color="red")
    ax.text(9.5, ys[-1]*0.2, "N > 7:\ncross-NUMA", fontsize=8, color="#800", alpha=0.6)
fig.suptitle("Exp 2: YCSB throughput vs clients per CN — Bell cluster", y=1.02, fontsize=13)
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp2_scaling.png"), bbox_inches="tight")
print("wrote exp2_scaling.png")

# ====== Exp 3: MN count ======
mn = {"workloada": {}, "workloadc": {}}
with open(os.path.join(HERE, "exp3_mn_count.csv")) as f:
    for r in csv.DictReader(f):
        if r["tpt_ops_per_sec"] == "0": continue
        mn[r["workload"]][int(r["memory_num"])] = int(r["tpt_ops_per_sec"])

fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
Ms = [1, 2, 3]; w = 0.35; x = np.arange(len(Ms))
ya = [mn["workloada"].get(m, 0)/1e6 for m in Ms]
yc = [mn["workloadc"].get(m, 0)/1e6 for m in Ms]
b1 = ax.bar(x - w/2, ya, w, label="YCSB-A (50R/50U)", color="#ff7f0e")
b2 = ax.bar(x + w/2, yc, w, label="YCSB-C (100R)",    color="#2ca02c")
for i, m in enumerate(Ms):
    ax.text(i - w/2, ya[i] + 0.03, f"{ya[i]:.2f}", ha="center", fontsize=9)
    ax.text(i + w/2, yc[i] + 0.03, f"{yc[i]:.2f}", ha="center", fontsize=9)
ax.set_xticks(x); ax.set_xticklabels([f"{m} MN" for m in Ms])
ax.set_ylabel("Throughput (M ops/s)")
ax.set_title("Exp 3: throughput vs MN count  (1 CN, 8 clients, num_idx_rep=1)\nNote: M=1 hits FUSEE's silent-insert-drop bug, numbers inflated")
ax.grid(True, axis="y", alpha=0.3); ax.legend()
ax.axvline(0.5, color="red", linestyle=":", alpha=0.5)
ax.text(0.05, max(yc)*0.95, "⚠ M=1 bug", fontsize=9, color="#800")
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp3_mn_count.png"))
print("wrote exp3_mn_count.png")

# ====== Exp 4: Replication factor ======
rep = {}   # (nrep, irep, wl) -> tpt
with open(os.path.join(HERE, "exp4_replication.csv")) as f:
    for r in csv.DictReader(f):
        if r["tpt_ops_per_sec"] == "0": continue
        rep[(int(r["num_replication"]), int(r["num_idx_rep"]), r["workload"])] = int(r["tpt_ops_per_sec"])

fig, (axa, axc) = plt.subplots(1, 2, figsize=(12, 4.5), dpi=130)
configs = [(2,1,"2/1"),(2,2,"2/2"),(3,1,"3/1"),(3,2,"3/2"),(3,3,"3/3")]
xlabels = [c[2] for c in configs]
x = np.arange(len(configs))
for ax, wl, title in [(axa, "workloada", "YCSB-A"), (axc, "workloadc", "YCSB-C")]:
    ys = [rep.get((c[0], c[1], wl), 0)/1e6 for c in configs]
    colors = ['#1f77b4' if c[1]==1 else '#ff7f0e' if c[1]==c[0] else '#9467bd' for c in configs]
    bars = ax.bar(x, ys, color=colors)
    for i, (c, y) in enumerate(zip(configs, ys)):
        ax.text(i, y + max(ys)*0.02, f"{y:.2f}", ha="center", fontsize=9)
    ax.set_xticks(x); ax.set_xticklabels(xlabels)
    ax.set_xlabel("num_replication / num_idx_rep")
    ax.set_ylabel("Throughput (M ops/s)")
    ax.set_title(f"{title}: 3 MN, 1 CN, 8 clients"); ax.grid(True, axis="y", alpha=0.3)
    ax.set_ylim(0, max(ys)*1.15)
# legend
from matplotlib.patches import Patch
leg = [Patch(facecolor='#1f77b4', label='num_idx_rep = 1'),
       Patch(facecolor='#9467bd', label='1 < num_idx_rep < num_replication'),
       Patch(facecolor='#ff7f0e', label='num_idx_rep = num_replication')]
axa.legend(handles=leg, fontsize=8, loc="upper right")
fig.suptitle("Exp 4: replication factor tradeoff", fontsize=13, y=1.02)
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp4_replication.png"), bbox_inches="tight")
print("wrote exp4_replication.png")

# ====== Exp 5: KV size ======
kv = {"workloada": {}, "workloadc": {}}
with open(os.path.join(HERE, "exp5_kvsize.csv")) as f:
    for r in csv.DictReader(f):
        if r["tpt_ops_per_sec"] == "0": continue
        kv[r["workload"]][int(r["subblock_size"])] = int(r["tpt_ops_per_sec"])

fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
sizes = [256, 512, 1024]; w = 0.35; x = np.arange(len(sizes))
ya = [kv["workloada"].get(s, 0)/1e6 for s in sizes]
yc = [kv["workloadc"].get(s, 0)/1e6 for s in sizes]
ax.bar(x - w/2, ya, w, label="YCSB-A", color="#ff7f0e")
ax.bar(x + w/2, yc, w, label="YCSB-C", color="#2ca02c")
for i, s in enumerate(sizes):
    if ya[i] > 0:
        ax.text(i - w/2, ya[i] + 0.03, f"{ya[i]:.2f}", ha="center", fontsize=9)
    else:
        ax.text(i - w/2, 0.1, "crash", ha="center", fontsize=9, color="red")
    ax.text(i + w/2, yc[i] + 0.03, f"{yc[i]:.2f}", ha="center", fontsize=9)
ax.set_xticks(x); ax.set_xticklabels([f"{s} B" for s in sizes])
ax.set_xlabel("subblock_size"); ax.set_ylabel("Throughput (M ops/s)")
ax.set_title("Exp 5: throughput vs KV slot size — 3 MN, 1 CN, 8 clients")
ax.grid(True, axis="y", alpha=0.3); ax.legend()
fig.tight_layout(); fig.savefig(os.path.join(OUT, "exp5_kvsize.png"))
print("wrote exp5_kvsize.png")

# ====== Print summaries ======
print("\n=== Summary ===")
print("Exp 1 latency (μs):")
for n in ["SEARCH","INSERT","UPDATE","DELETE"]:
    a = data[n][0]
    print(f"  {n:<7} avg={a.mean():.2f}  p50={pct(a,50)}  p99={pct(a,99)}")
print("Exp 2 scaling peak:")
print(f"  YCSB-A max: {max(scaling['workloada'].values())/1e6:.3f} M (N={max(scaling['workloada'], key=scaling['workloada'].get)})")
print(f"  YCSB-C max: {max(scaling['workloadc'].values())/1e6:.3f} M (N={max(scaling['workloadc'], key=scaling['workloadc'].get)})")
print("Exp 3 MN count (YCSB-C):")
for m in [1, 2, 3]:
    print(f"  M={m}: {mn['workloadc'].get(m, 0)/1e6:.3f} M")
print("Exp 4 rep factor (YCSB-A peak):", max((v for k, v in rep.items() if k[2]=="workloada"), default=0)/1e6, "M")
print("Exp 5 subblock (YCSB-C):")
for s in [256, 512, 1024]:
    print(f"  {s}: {kv['workloadc'].get(s, 0)/1e6:.3f} M")
