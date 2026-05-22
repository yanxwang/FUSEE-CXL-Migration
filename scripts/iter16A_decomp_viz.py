#!/usr/bin/env python3
"""iter-16A xhost write probe-on decomp sweep — visualization.

Reads aggregate.csv (output of iter16A_xhost_decomp_sweep.sh) and produces:
  - 8 per-stage plots (MANDATORY): {Stage1..Stage8}_T_p50p99.png
  - 3 aggregate plots: StageW_T_p50p99.png, StageR_T_p50p99.png, RTT_T_p50p99.png
  - 3 summary plots:
      worker_stages_overlay.png   — all 5 worker stages on one chart
      receiver_stages_overlay.png — all 3 receiver stages on one chart
      stage_contribution_stacked.png — stacked bar of stage contribution to StageW per T

Stages 3, 4, 8 (small CPU-side ops < 100 ns) are annotated as
"probe-overhead-bound" — their measured value is dominated by probe cost.
"""
import csv, sys, os, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: iter16A_decomp_viz.py <aggregate.csv>"); sys.exit(1)

CSV = sys.argv[1]
OUT_DIR = os.path.dirname(os.path.abspath(CSV))

STAGES = [
    ("Stage1", "slot_reserve",  False),
    ("Stage2", "slot_wait",     False),
    ("Stage3", "value_xfer",    True),   # probe-overhead-bound
    ("Stage4", "ctrl_publish",  True),
    ("Stage5", "ack_wait",      False),
    ("Stage6", "rcv_poll",      False),
    ("Stage7", "rcv_work",      False),
    ("Stage8", "ack_publish",   True),
]
WORKER_STAGES = ["Stage1", "Stage2", "Stage3", "Stage4", "Stage5"]
RECV_STAGES = ["Stage6", "Stage7", "Stage8"]
AGG = [("StageW", "worker total (sum of 1..5)"),
       ("StageR", "receiver total (sum of 6..8)"),
       ("RTT", "cross-host RTT (StageW − StageR)")]

# Load
data = defaultdict(lambda: defaultdict(list))  # T -> metric -> [vals across reps]
thpt = defaultdict(list)
with open(CSV) as f:
    r = csv.DictReader(f)
    for row in r:
        T = int(row["T"])
        for k, v in row.items():
            if k in ("T", "rep"): continue
            try:
                data[T][k].append(float(v))
            except (ValueError, TypeError):
                pass
        try:
            thpt[T].append(float(row["thpt_Mops"]))
        except (ValueError, KeyError):
            pass

Ts = sorted(data.keys())
if not Ts:
    print("no data"); sys.exit(1)


def med(vals):
    return statistics.median(vals) if vals else 0


def per_stage_plot(stage_id, stage_label, overhead_bound):
    p50_vals = [med(data[T].get(f"{stage_id.lower()}_p50_ns", [])) for T in Ts]
    p99_vals = [med(data[T].get(f"{stage_id.lower()}_p99_ns", [])) for T in Ts]
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.plot(Ts, p50_vals, marker="o", linewidth=2, color="#1f77b4", label="p50")
    ax.plot(Ts, p99_vals, marker="s", linewidth=2, color="#d62728", label="p99")
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("T (workers per host)")
    ax.set_ylabel("Latency (ns)")
    title = f"{stage_id} = {stage_label} — probe-on, V=1024, xhost_write, 3 reps median"
    if overhead_bound:
        title += "\n[CAVEAT: probe-overhead-bound, treat as upper-bound]"
    ax.set_title(title, fontsize=10)
    ax.grid(True, alpha=0.3, which="both"); ax.legend()
    out = os.path.join(OUT_DIR, f"{stage_id}_T_p50p99.png")
    plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
    print(f"wrote {out}")


def overlay_plot(stage_ids, title, fname):
    fig, ax = plt.subplots(figsize=(8, 5))
    colors = plt.cm.tab10.colors
    for i, sid in enumerate(stage_ids):
        p50 = [med(data[T].get(f"{sid.lower()}_p50_ns", [])) for T in Ts]
        ax.plot(Ts, p50, marker="o", linewidth=2,
                color=colors[i % 10], label=f"{sid} p50")
    ax.set_xscale("log", base=2); ax.set_yscale("log")
    ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("T (workers per host)"); ax.set_ylabel("Latency (ns, log)")
    ax.set_title(title, fontsize=11)
    ax.grid(True, alpha=0.3, which="both"); ax.legend(fontsize=9)
    out = os.path.join(OUT_DIR, fname)
    plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
    print(f"wrote {out}")


def stacked_bar():
    fig, ax = plt.subplots(figsize=(9, 5.5))
    bottom = [0.0] * len(Ts)
    colors = plt.cm.tab10.colors
    for i, sid in enumerate(WORKER_STAGES):
        vals = [med(data[T].get(f"{sid.lower()}_p50_ns", [])) for T in Ts]
        ax.bar([str(t) for t in Ts], vals, bottom=bottom,
               color=colors[i % 10], label=sid)
        bottom = [b + v for b, v in zip(bottom, vals)]
    ax.set_xlabel("T (workers per host)")
    ax.set_ylabel("Latency p50 (ns) — stacked by stage")
    ax.set_title("Worker stage contribution stacked vs T (probe-on, V=1024)", fontsize=11)
    ax.legend(fontsize=9); ax.grid(True, alpha=0.3, axis="y")
    out = os.path.join(OUT_DIR, "stage_contribution_stacked.png")
    plt.tight_layout(); plt.savefig(out, dpi=140); plt.close()
    print(f"wrote {out}")


# --- 8 mandatory per-stage plots ---
for sid, label, ovh in STAGES:
    per_stage_plot(sid, label, ovh)

# --- 3 aggregate (StageW, StageR, RTT) ---
for sid, label in AGG:
    per_stage_plot(sid, label, False)

# --- 3 summary plots ---
overlay_plot(WORKER_STAGES, "All worker stages p50 vs T (log-log)",
             "worker_stages_overlay.png")
overlay_plot(RECV_STAGES, "All receiver stages p50 vs T (log-log)",
             "receiver_stages_overlay.png")
stacked_bar()

# Text summary
out = os.path.join(OUT_DIR, "decomp_summary.txt")
with open(out, "w") as f:
    f.write("iter-16A xhost_write probe-on decomp sweep — aggregate (median across reps)\n")
    f.write("=" * 90 + "\n")
    hdr_fmt = "{:<6} " + " ".join(["{:>10}"] * 11) + "\n"
    f.write(hdr_fmt.format("T", *[s[0] for s in STAGES] + ["StageW", "StageR", "RTT"]))
    f.write("p50 (ns):\n")
    for T in Ts:
        row = [str(T)]
        for sid in [s[0] for s in STAGES] + ["StageW", "StageR", "RTT"]:
            v = med(data[T].get(f"{sid.lower()}_p50_ns", []))
            row.append(f"{v:.0f}")
        f.write(hdr_fmt.format(*row))
    f.write("\nrates (%):\n")
    f.write(f"{'T':<6} {'XWS2R':>8} {'XWS5T':>8} {'XWR6Z gap':>10} {'thpt_Mops':>10}\n")
    for T in Ts:
        s2r = med(data[T].get("xws2r_rate_pct", []))
        s5t = med(data[T].get("xws5t_rate_pct", []))
        gap = med(data[T].get("gap_rate_pct", []))
        tp = med(thpt.get(T, []))
        f.write(f"{T:<6} {s2r:>8.4f} {s5t:>8.4f} {gap:>10.4f} {tp:>10.3f}\n")
print(f"wrote {out}")
