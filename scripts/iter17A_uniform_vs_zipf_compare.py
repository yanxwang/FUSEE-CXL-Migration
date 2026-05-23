#!/usr/bin/env python3
"""iter-17A uniform vs zipf-0.99 8-group comparison."""
import csv, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT_DIR = "/home/yanwang/FUSEE/docs/iter17A_scaling_8group_uniform_20260523_000353"
os.makedirs(OUT_DIR, exist_ok=True)

# Sweep dirs
SWEEP_ZIPF = [
    "/home/yanwang/FUSEE/docs/iter17A_scaling_finals_20260522_084104",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_fill_20260522_081401",
    "/home/yanwang/FUSEE/docs/iter17A_scaling_AvsB_20260522_075307",
]
SWEEP_UNIFORM = ["/home/yanwang/FUSEE/docs/iter17A_scaling_8group_uniform_20260523_000353"]

def collect(sweep_dirs):
    data = defaultdict(lambda: defaultdict(list))  # [(N, routing)][T] = [thpt, ...]
    for d in sweep_dirs:
        csv_path = os.path.join(d, "grid.csv")
        if not os.path.exists(csv_path): continue
        with open(csv_path) as f:
            for r in csv.DictReader(f):
                try:
                    T = int(r["T"])
                    N = int(r["N"])
                    routing = r["routing"]
                    thpt_col = "thpt" if "thpt" in r else ("thpt_cluster_ops" if "thpt_cluster_ops" in r else "thpt_h0_ops")
                    thpt = float(r[thpt_col]) / 1e6
                    if thpt > 0:
                        data[(N, routing)][T].append(thpt)
                except (KeyError, ValueError):
                    continue
    return {k: {T: statistics.median(v) for T, v in by_T.items() if v} for k, by_T in data.items()}

zipf = collect(SWEEP_ZIPF)
uniform = collect(SWEEP_UNIFORM)

# iter-15A baseline zipf-0.99 from phase2 CSV
def iter15A_zipf():
    csv_path = "/home/yanwang/FUSEE/docs/iter15A_microbench_phase2_20260520_063314/grid.csv"
    by_T = defaultdict(list)
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            if r["scenario"] != "xhost_write": continue
            if r["keydist"] != "zipf-0.99": continue
            if r["V"] != "1024": continue
            T = int(r["T"])
            by_T[T].append(float(r["thpt_Mops"]))
    return {T: statistics.median(v) for T, v in by_T.items()}

iter15A = iter15A_zipf()

Ts = [1, 2, 4, 8, 16, 32, 64]

# Build 8-group dicts
def make_groups(data, label_prefix, has_iter15A=True):
    g = [None] * 9  # 1-indexed
    if has_iter15A:
        g[1] = iter15A
    g[2] = data.get((0, "worker_id"), {})  # iter-17A path opt N=0
    g[3] = g[2]                             # Plan A worker_id N=0
    g[4] = data.get((4, "worker_id"), {})
    g[5] = data.get((8, "worker_id"), {})
    g[6] = data.get((0, "key_hash"), {})
    g[7] = data.get((4, "key_hash"), {})
    g[8] = data.get((8, "key_hash"), {})
    return g

g_zipf    = make_groups(zipf,    "zipf",    has_iter15A=True)
g_uniform = make_groups(uniform, "uniform", has_iter15A=False)

# ----- CSV side-by-side table -----
csv_path = os.path.join(OUT_DIR, "uniform_vs_zipf_table.csv")
with open(csv_path, "w") as f:
    f.write("Group,Dist," + ",".join(f"T={T}" for T in Ts) + "\n")
    labels = [
        "1. iter-15A baseline",
        "2. iter-17A path opt N=0",
        "3. Plan A worker_id N=0",
        "4. Plan A worker_id N=4",
        "5. Plan A worker_id N=8",
        "6. Plan B key_hash  N=0",
        "7. Plan B key_hash  N=4",
        "8. Plan B key_hash  N=8",
    ]
    for i in range(1, 9):
        row_z = labels[i-1] + ",zipf-0.99"
        row_u = labels[i-1] + ",uniform"
        for T in Ts:
            vz = g_zipf[i].get(T) if g_zipf[i] else None
            vu = g_uniform[i].get(T) if g_uniform[i] else None
            row_z += f",{vz:.3f}" if vz is not None else ",NA"
            row_u += f",{vu:.3f}" if vu is not None else ",NA"
        f.write(row_z + "\n")
        f.write(row_u + "\n")
print(f"wrote {csv_path}")

# ----- Figure: side-by-side log + linear, with both dist on each panel -----
groups_meta = [
    (2, "2. iter-17A path opt N=0", "#1f77b4", "s"),
    (4, "4. Plan A wid N=4", "#d62728", "^"),
    (5, "5. Plan A wid N=8", "#d62728", "v"),
    (7, "7. Plan B kh N=4", "#2ca02c", "^"),
    (8, "8. Plan B kh N=8", "#9467bd", "v"),
]

fig, axes = plt.subplots(1, 2, figsize=(20, 8))
for ax_idx, (ax, yscale_log) in enumerate([(axes[0], True), (axes[1], False)]):
    for gi, label, color, marker in groups_meta:
        z_ys = [g_zipf[gi].get(T, float("nan")) for T in Ts]
        u_ys = [g_uniform[gi].get(T, float("nan")) for T in Ts]
        ax.plot(Ts, z_ys, color=color, marker=marker, linestyle="-",
                linewidth=2, markersize=8, label=f"{label} (zipf)")
        ax.plot(Ts, u_ys, color=color, marker=marker, linestyle=":",
                linewidth=2, markersize=8, label=f"{label} (uniform)")
    # iter-15A baseline (zipf only)
    z1 = [g_zipf[1].get(T, float("nan")) for T in Ts]
    ax.plot(Ts, z1, color="#888888", marker="o", linewidth=2, label="1. iter-15A baseline (zipf)")
    # 20 Mops target
    ax.axhline(y=20, color="#aa00aa", linestyle="--", alpha=0.7, linewidth=2)
    ax.text(1.05, 21 if yscale_log else 19, "20 Mops/s target",
            fontsize=10, color="#aa00aa", fontweight="bold",
            va="bottom" if yscale_log else "top")
    ax.set_xscale("log", base=2)
    if yscale_log:
        ax.set_yscale("log")
        ax.set_ylabel("Cluster throughput (Mops/s, log)", fontsize=11)
        ax.set_title("log-Y", fontsize=12)
    else:
        ax.set_ylim(0, 22)
        ax.set_ylabel("Cluster throughput (Mops/s, linear)", fontsize=11)
        ax.set_title("linear-Y", fontsize=12)
    ax.set_xticks(Ts)
    ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("T (workers per host)", fontsize=11)
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="upper left", fontsize=8, ncol=2)

fig.suptitle("iter-17A: uniform (dotted) vs zipf-0.99 (solid) — xhost_write V=1024 (cluster Mops/s, 3-rep median)",
             fontsize=13, y=0.99)
plt.tight_layout(rect=[0, 0, 1, 0.96])
out_fig = os.path.join(OUT_DIR, "uniform_vs_zipf_compare.png")
plt.savefig(out_fig, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out_fig}")

# ----- Heatmap: uniform / zipf ratio per cell -----
labels_short = ["iter-17A N=0", "A N=4", "A N=8", "B N=4", "B N=8"]
mat = np.zeros((5, len(Ts)))
for i, (gi, *_) in enumerate(groups_meta):
    for j, T in enumerate(Ts):
        z = g_zipf[gi].get(T)
        u = g_uniform[gi].get(T)
        if z and z > 0 and u is not None:
            mat[i, j] = u / z
        else:
            mat[i, j] = float("nan")

fig, ax = plt.subplots(1, 1, figsize=(12, 4))
im = ax.imshow(mat, aspect="auto", cmap="RdYlGn", vmin=0.5, vmax=3.5)
ax.set_xticks(range(len(Ts)))
ax.set_xticklabels([f"T={T}" for T in Ts])
ax.set_yticks(range(5))
ax.set_yticklabels(labels_short)
ax.set_title("uniform/zipf-0.99 throughput ratio — >1 = uniform wins (hot-key relief)",
             fontsize=12, pad=10)
for i in range(5):
    for j in range(len(Ts)):
        v = mat[i, j]
        if not np.isnan(v):
            color = "black" if 0.8 < v < 2.5 else "white"
            ax.text(j, i, f"{v:.2f}×", ha="center", va="center",
                    color=color, fontsize=10, fontweight="bold")
plt.colorbar(im, ax=ax, label="uniform / zipf ratio")
plt.tight_layout()
out_hm = os.path.join(OUT_DIR, "uniform_vs_zipf_ratio_heatmap.png")
plt.savefig(out_hm, dpi=140, bbox_inches="tight")
plt.close()
print(f"wrote {out_hm}")

# ----- Print summary -----
print()
print(f"{'Cell':<20s}  {'zipf-0.99':>10s}  {'uniform':>10s}  {'u/z':>6s}")
for gi, label, *_ in groups_meta:
    print(f"--- {label} ---")
    for T in Ts:
        z = g_zipf[gi].get(T)
        u = g_uniform[gi].get(T)
        z_s = f"{z:.3f}" if z else "NA"
        u_s = f"{u:.3f}" if u else "NA"
        r_s = f"{u/z:.2f}×" if (z and z > 0 and u) else "NA"
        print(f"  T={T:<3d}            {z_s:>10s}  {u_s:>10s}  {r_s:>6s}")
