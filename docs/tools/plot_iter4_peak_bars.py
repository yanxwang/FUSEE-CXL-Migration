#!/usr/bin/env python3
"""iter-4 two-bar-chart deliverable.

For each YCSB workload (A,B,C,D,F), plot 4 bars over the kv-8/256/512/1024
sweeps:
  fig 1  peak throughput (Mops/s, cache=on)
  fig 2  p50 latency at the peak-throughput T (microseconds)

Latency choice per workload:
  C / D  -> read p50  (read-heavy or read-only)
  A / B / F -> write p50 (write/RMW-heavy)
"""
import re, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_style import apply_style, STYLE_B4, COLORS, bar_with_headroom  # noqa: E402
apply_style()

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"(?:value_size=(?P<vs>\d+)\s+)?"
    r"num_hosts=\d+\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=\d+\s+trans_wall_max=[\d.]+s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)\s+"
    r"w_avg_ns=\d+\s+w_p50_ns=(?P<wp50>\d+)\s+w_p99_ns=\d+\s+"
    r"r_avg_ns=\d+\s+r_p50_ns=(?P<rp50>\d+)\s+r_p99_ns=\d+"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)_opt[ABC]_t\d+_cache(?P<c>on|off)")

WORKLOADS = ["workloada", "workloadb", "workloadc", "workloadd", "workloadf"]
WL_LABEL  = ["A", "B", "C", "D", "F"]
USE_READ_P50 = {"workloadc", "workloadd"}

SWEEPS = [
    (8,    "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv8_20260424_173316/SUMMARY.log"),
    (256,  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv256_20260424_180808/SUMMARY.log"),
    (512,  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv512_20260424_184334/SUMMARY.log"),
    (1024, "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv1024_20260424_191839/SUMMARY.log"),
]

def parse_peak(path, want_cache=1):
    """For each workload, return (peak_thpt_mops, T_at_peak, w_p50_us, r_p50_us)."""
    best = {wl: (0.0, 0, 0.0, 0.0) for wl in WORKLOADS}
    with open(path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m or m.group("opt") != "C" or int(m.group("c")) != want_cache:
                continue
            t = TAG.search(line)
            if not t: continue
            wl = t.group("wl")
            if wl not in best: continue
            thpt = float(m.group("thpt")) / 1e6
            if thpt > best[wl][0]:
                best[wl] = (thpt,
                            int(m.group("T")),
                            int(m.group("wp50")) / 1000.0,
                            int(m.group("rp50")) / 1000.0)
    return best

def main():
    out_dir = "/home/yanwang/FUSEE/docs/iter4_kv_compare"
    os.makedirs(out_dir, exist_ok=True)
    by_size = [(vs, parse_peak(p, 1)) for vs, p in SWEEPS]

    width = 0.18
    x_pos = list(range(len(WORKLOADS)))
    # Style B4: 4-tone greyscale + accent. kv1024 = accent.
    colors = STYLE_B4

    # ---- Figure 1: peak throughput ----
    fig, ax = plt.subplots(figsize=(9, 5))
    for i, (vs, peaks) in enumerate(by_size):
        ys = [peaks[wl][0] for wl in WORKLOADS]
        offsets = [p + (i - (len(by_size)-1)/2) * width for p in x_pos]
        ax.bar(offsets, ys, width=width, label=f"kv={vs} B", color=colors[i])
        for off, y in zip(offsets, ys):
            ax.text(off, y + 0.6, f"{y:.1f}", ha="center", va="bottom", fontsize=7)
    ax.axhline(20, linestyle="--", color=COLORS["target"], linewidth=1,
               label="target 20 Mops/s")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(WL_LABEL)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("peak trans_agg_thpt (Mops/s, cache=on)")
    ax.set_title("iter-4 — peak throughput per workload (4 value sizes)")
    ax.legend(fontsize=8, loc="upper right")
    bar_with_headroom(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "iter4_peak_thpt_bars.png"))
    plt.close(fig)

    # ---- Figure 2: p50 latency at peak T ----
    fig, ax = plt.subplots(figsize=(9, 5))
    for i, (vs, peaks) in enumerate(by_size):
        ys = []
        for wl in WORKLOADS:
            thpt, T, wp50, rp50 = peaks[wl]
            ys.append(rp50 if wl in USE_READ_P50 else wp50)
        offsets = [p + (i - (len(by_size)-1)/2) * width for p in x_pos]
        ax.bar(offsets, ys, width=width, label=f"kv={vs} B", color=colors[i])
        for off, y in zip(offsets, ys):
            if y > 0:
                ax.text(off, y + 0.15, f"{y:.1f}", ha="center", va="bottom", fontsize=7)
    ax.set_xticks(x_pos)
    ax.set_xticklabels(WL_LABEL)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("p50 latency at peak-throughput T (us)")
    ax.set_title("iter-4 — p50 latency at peak T per workload (cache=on)\n"
                 "C/D = read p50, A/B/F = write p50")
    ax.legend(fontsize=8, loc="upper left")
    bar_with_headroom(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "iter4_peak_p50_bars.png"))
    plt.close(fig)

    # Print the underlying numbers so the caller can sanity-check.
    print("workload | kv8           | kv256         | kv512         | kv1024")
    print("---------|---------------|---------------|---------------|--------------")
    for wl, lbl in zip(WORKLOADS, WL_LABEL):
        cells = []
        for vs, peaks in by_size:
            thpt, T, wp50, rp50 = peaks[wl]
            p50 = rp50 if wl in USE_READ_P50 else wp50
            cells.append(f"{thpt:5.2f}@T{T:<2} p50={p50:5.1f}")
        print(f"   {lbl}     | " + " | ".join(cells))

if __name__ == "__main__":
    main()
