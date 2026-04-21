#!/usr/bin/env python3
"""Plot YCSB scaling results in FUSEE paper Figure 13 style.

Figure 13 shows throughput vs number of clients for YCSB-A and YCSB-C,
comparing FUSEE, Clover, pDPM-Direct. We don't have Clover/pDPM-Direct on
this hardware, so we plot only our FUSEE-on-c1/c2 results, but format the
plot so later measurements from other systems can be overlaid.
"""
import os
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------- Load Plan C scaling data ----------
scaling = {"workloada": {}, "workloadc": {}}
with open(os.path.join(HERE, "scaling_results.csv")) as f:
    r = csv.DictReader(f)
    for row in r:
        wl = row["workload"]
        n  = int(row["num_clients"])
        if row["tpt_ops_per_sec"] == "0":  # failed
            continue
        tpt = int(row["tpt_ops_per_sec"])
        scaling[wl][n] = tpt

# ---------- Load Plan B ----------
plan_b = {}  # workload -> list of (cn_id, tpt, failed)
with open(os.path.join(HERE, "plan_b_results.csv")) as f:
    r = csv.DictReader(f)
    for row in r:
        wl = row["workload"]
        plan_b.setdefault(wl, []).append({
            "cn": row["cn"], "tpt": int(row["tpt"]),
            "failed": int(row["failed"]),
        })

# ---------- Figure 13-style plot ----------
fig, (axa, axc) = plt.subplots(1, 2, figsize=(12, 4.5), dpi=130)

for ax, wl, title in [(axa, "workloada", "YCSB-A (50% read / 50% update)"),
                      (axc, "workloadc", "YCSB-C (100% read)")]:
    pts = sorted(scaling[wl].items())
    xs = [p[0] for p in pts]
    ys = [p[1] / 1e6 for p in pts]
    ax.plot(xs, ys, "o-", color="#1f77b4", linewidth=2, markersize=7,
            label="FUSEE @ BlueField-3 (1 CN × N clients)")

    # Plan B (2 CNs × 8 clients = 16 clients total, sum of successful ops)
    if wl in plan_b:
        total_tpt = sum(r["tpt"] for r in plan_b[wl])
        ax.plot([16], [total_tpt / 1e6], "s", color="#d62728", markersize=9,
                label=f"FUSEE @ BlueField-3 (2 CNs × 8 clients, Plan B)")
        # annotate
        ax.annotate(f"{total_tpt/1e6:.2f} M",
                    xy=(16, total_tpt/1e6), xytext=(16, total_tpt/1e6 + 0.3),
                    fontsize=8, ha="center", color="#d62728")

    ax.set_xscale("log", base=2)
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlabel("Number of clients")
    ax.set_ylabel("Throughput (M ops/s)")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)

    for x, y in zip(xs, ys):
        ax.annotate(f"{y:.2f}", xy=(x, y), xytext=(x, y*1.08),
                    fontsize=8, ha="center", color="#1f77b4")

fig.suptitle("YCSB throughput on c1/c2 (2 MN + 1 CN, FUSEE num_replication=2)",
             fontsize=13, y=1.02)
fig.tight_layout()
fig.savefig(os.path.join(HERE, "ycsb_scaling.png"), bbox_inches="tight")
print("wrote ycsb_scaling.png")

# ---------- Speedup plot (vs ideal linear) ----------
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
for wl, color, label in [("workloada", "#ff7f0e", "YCSB-A"),
                         ("workloadc", "#2ca02c", "YCSB-C")]:
    pts = sorted(scaling[wl].items())
    if not pts: continue
    x0, y0 = pts[0]  # baseline
    xs = [p[0] for p in pts]
    ys = [p[1] / y0 for p in pts]
    ax.plot(xs, ys, "o-", color=color, linewidth=2, markersize=7, label=label)
    # ideal
    ideal = [x / x0 for x in xs]
    ax.plot(xs, ideal, "--", color=color, alpha=0.4, linewidth=1,
            label=f"{label} ideal linear")
ax.set_xscale("log", base=2)
ax.set_yscale("log", base=2)
ax.set_xlabel("Number of clients")
ax.set_ylabel(f"Throughput speedup vs {x0}-client baseline")
ax.set_title("YCSB scaling efficiency (1 CN, c2)")
ax.grid(True, which="both", alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig(os.path.join(HERE, "ycsb_speedup.png"))
print("wrote ycsb_speedup.png")

# ---------- Plan B breakdown ----------
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=130)
wl_names = ["workloada", "workloadc"]
x_pos = np.arange(len(wl_names))
w = 0.3
cna = [next(r["tpt"] for r in plan_b[wl] if r["cn"] == "A")/1e6 for wl in wl_names]
cnb = [next(r["tpt"] for r in plan_b[wl] if r["cn"] == "B")/1e6 for wl in wl_names]

ax.bar(x_pos - w/2, cna, w, label="CN_A (c1, init)", color="#1f77b4")
ax.bar(x_pos + w/2, cnb, w, label="CN_B (c2, staggered 3s)", color="#d62728")
for i, (a, b) in enumerate(zip(cna, cnb)):
    ax.text(i - w/2, a + 0.05, f"{a:.2f}M", ha="center", fontsize=9)
    ax.text(i + w/2, b + 0.05, f"{b:.2f}M", ha="center", fontsize=9)
    ax.text(i, max(a, b) + 0.5, f"Σ = {a+b:.2f}M", ha="center", fontsize=10, weight="bold")
ax.set_xticks(x_pos)
ax.set_xticklabels(["YCSB-A\n(50R/50U)", "YCSB-C\n(100R)"])
ax.set_ylabel("Throughput (M ops/s)")
ax.set_title("Plan B: 2 MN + 2 CN × 8 clients — per-CN throughput\n(CN_B shows high FAIL_RETRY due to load-phase collision)")
ax.grid(True, axis="y", alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig(os.path.join(HERE, "plan_b_breakdown.png"))
print("wrote plan_b_breakdown.png")

# ---------- summary text ----------
print()
print("=== Plan C scaling summary ===")
for wl in ["workloada", "workloadc"]:
    print(f"  {wl}:")
    for n, t in sorted(scaling[wl].items()):
        print(f"    N={n:3d}: {t/1e6:.3f} M ops/s")
print()
print("=== Plan B summary ===")
for wl, rs in plan_b.items():
    total = sum(r["tpt"] for r in rs)
    total_fail = sum(r["failed"] for r in rs)
    print(f"  {wl}: CN_A={rs[0]['tpt']/1e6:.3f}M  CN_B={rs[1]['tpt']/1e6:.3f}M"
          f"  Σ={total/1e6:.3f}M  failed={total_fail}")
