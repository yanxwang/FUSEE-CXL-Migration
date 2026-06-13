#!/usr/bin/env python3
"""Plot Protocol F YCSB sweep results.

Reads docs/protocol_F_sweep_<timestamp>/results.csv and produces:
  thpt_vs_T.png       - aggregate cluster throughput vs T, log + linear panels
  latency_vs_T.png    - w_p50/w_p99 and r_p50/r_p99 vs T
  pool_F_summary.md   - markdown summary table for the design doc

Usage:
  python3 scripts/plot_protocol_F_sweep.py <results.csv>
"""
import csv, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_F_sweep.py <results.csv>")
    sys.exit(1)
CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

rows = []
with open(CSV) as f:
    for r in csv.DictReader(f):
        if r["trans_thpt"] == "FAIL": continue
        rows.append({
            "T": int(r["T"]),
            "N": int(r["N"]),
            "wl": r["workload"],
            "load_thpt": float(r["load_thpt"]),
            "trans_thpt": float(r["trans_thpt"]),
            "w_p50": float(r["w_p50_ns"]) / 1000,  # to µs
            "w_p99": float(r["w_p99_ns"]) / 1000,
            "r_p50": float(r["r_p50_ns"]) / 1000,
            "r_p99": float(r["r_p99_ns"]) / 1000,
        })

def get(rows, wl, N):
    return sorted([r for r in rows if r["wl"] == wl and r["N"] == N],
                  key=lambda r: r["T"])

workloads = sorted({r["wl"] for r in rows})
fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))

for wl in workloads:
    for N in (1, 2):
        d = get(rows, wl, N)
        if not d: continue
        Ts = [r["T"] for r in d]
        ttp_mops = [r["trans_thpt"] / 1e6 for r in d]  # already cluster aggregate
        label = f"{wl} N={N}"
        ls = "-" if N == 2 else "--"
        axes[0].plot(Ts, ttp_mops, marker="o", linestyle=ls, label=label)
        axes[1].plot(Ts, ttp_mops, marker="o", linestyle=ls, label=label)

axes[0].set_xscale("log", base=2); axes[0].set_yscale("log")
axes[0].set_xlabel("T (threads / host)")
axes[0].set_ylabel("Cluster throughput (Mops/s, log)")
axes[0].set_title("Protocol F YCSB scaling — log")
axes[0].grid(True, alpha=0.3, which="both")
axes[0].legend(fontsize=9)

axes[1].set_xlabel("T (threads / host)")
axes[1].set_ylabel("Cluster throughput (Mops/s)")
axes[1].set_title("Protocol F YCSB scaling — linear")
axes[1].grid(True, alpha=0.3)
axes[1].legend(fontsize=9)

fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "thpt_vs_T.png"), dpi=140)
plt.close(fig)
print(f"wrote thpt_vs_T.png")

# Latency: w_p50, w_p99, r_p50, r_p99 — one subplot per workload, with N=1 vs N=2
fig, axes = plt.subplots(1, len(workloads), figsize=(7 * len(workloads), 5))
if len(workloads) == 1: axes = [axes]
for ax, wl in zip(axes, workloads):
    for N in (1, 2):
        d = get(rows, wl, N)
        if not d: continue
        Ts = [r["T"] for r in d]
        ax.plot(Ts, [r["w_p50"] for r in d], "o--" if N == 1 else "o-",
                color="tab:red",   label=f"w_p50 N={N}")
        ax.plot(Ts, [r["w_p99"] for r in d], "s--" if N == 1 else "s-",
                color="tab:red",   alpha=0.5, label=f"w_p99 N={N}")
        ax.plot(Ts, [r["r_p50"] for r in d], "o--" if N == 1 else "o-",
                color="tab:blue",  label=f"r_p50 N={N}")
        ax.plot(Ts, [r["r_p99"] for r in d], "s--" if N == 1 else "s-",
                color="tab:blue",  alpha=0.5, label=f"r_p99 N={N}")
    ax.set_xscale("log", base=2); ax.set_yscale("log")
    ax.set_xlabel("T (threads / host)")
    ax.set_ylabel("Latency (µs, log)")
    ax.set_title(f"Protocol F — {wl}")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(fontsize=8, ncol=2)

fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "latency_vs_T.png"), dpi=140)
plt.close(fig)
print(f"wrote latency_vs_T.png")

# Markdown summary
with open(os.path.join(OUT_DIR, "summary.md"), "w") as f:
    f.write("# Protocol F YCSB sweep summary\n\n")
    f.write(f"Source: `{CSV}`\n\n")
    for wl in workloads:
        for N in (1, 2):
            d = get(rows, wl, N)
            if not d: continue
            peak = max(d, key=lambda r: r["trans_thpt"])
            peak_cluster = peak["trans_thpt"] / 1e6
            f.write(f"## {wl} N={N}\n")
            f.write(f"- Peak cluster throughput: **{peak_cluster:.2f} Mops/s** at T={peak['T']}\n")
            f.write(f"- At peak: w_p99={peak['w_p99']:.1f} µs, r_p99={peak['r_p99']:.1f} µs\n\n")

            f.write("| T | cluster thpt (Mops/s) | per-host (Kops/s) | w_p50 µs | w_p99 µs | r_p50 µs | r_p99 µs |\n")
            f.write("|---|---|---|---|---|---|---|\n")
            for r in d:
                f.write(f"| {r['T']} | {r['trans_thpt']/1e6:.3f} "
                        f"| {r['trans_thpt']/1e3/N:.1f} "
                        f"| {r['w_p50']:.1f} | {r['w_p99']:.1f} "
                        f"| {r['r_p50']:.1f} | {r['r_p99']:.1f} |\n")
            f.write("\n")
print("wrote summary.md")
