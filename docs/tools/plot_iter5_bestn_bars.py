#!/usr/bin/env python3
"""iter-5 best-N bars (per user request).

For each YCSB workload (A,B,C,D,F), 3 bars showing the BEST-across-N
(N ∈ {1,2,4}) at kv ∈ {256, 512, 1024}.

  Fig 1: peak throughput (Mops/s, cache=on) — pick max across N
  Fig 2: p50 latency at the peak-throughput cell (us)

Latency convention (matches iter-4 plot):
  C / D  -> read p50  (read-heavy / read-only)
  A / B / F -> write p50 (write/RMW-heavy)
"""
import re, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_style import apply_style, COLORS, bar_with_headroom  # noqa: E402
apply_style()

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"(?:value_size=(?P<vs>\d+)\s+)?"
    r"(?:num_flushers=(?P<nf>\d+)\s+)?"
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

# (vsize, N) → SUMMARY.log path
SWEEPS = {
    (256, 1):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv256_N1_20260424_233353/SUMMARY.log",
    (256, 2):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv256_N2_20260425_000926/SUMMARY.log",
    (256, 4):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv256_N4_20260425_004507/SUMMARY.log",
    (512, 1):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv512_N1_20260425_012027/SUMMARY.log",
    (512, 2):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv512_N2_20260425_015544/SUMMARY.log",
    (512, 4):  "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv512_N4_20260425_023057/SUMMARY.log",
    (1024, 1): "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv1024_N1_20260425_030542/SUMMARY.log",
    (1024, 2): "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv1024_N2_20260425_034129/SUMMARY.log",
    (1024, 4): "/home/yanwang/FUSEE/logs/g34_scaling_sweep_C_only_kv1024_N4_20260425_041619/SUMMARY.log",
}

VSIZES = [256, 512, 1024]
NS = [1, 2, 4]

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
    out_dir = "/home/yanwang/FUSEE/docs/iter5_kv_n_compare"
    os.makedirs(out_dir, exist_ok=True)

    # peaks_by_vs[vs] = { wl: (best_thpt, best_N, T_at_peak, w_p50, r_p50) }
    peaks_by_vs = {}
    for vs in VSIZES:
        per_wl = {wl: (0.0, 0, 0, 0.0, 0.0) for wl in WORKLOADS}
        for N in NS:
            p = parse_peak(SWEEPS[(vs, N)], 1)
            for wl in WORKLOADS:
                thpt, T, wp50, rp50 = p[wl]
                if thpt > per_wl[wl][0]:
                    per_wl[wl] = (thpt, N, T, wp50, rp50)
        peaks_by_vs[vs] = per_wl

    width = 0.27
    x_pos = list(range(len(WORKLOADS)))
    # Style B: kv256 light grey, kv512 mid grey, kv1024 accent red.
    vs_color = {256: COLORS["kv256"], 512: COLORS["kv512"],
                1024: COLORS["kv1024"]}

    # ---- Figure 1: best-N peak throughput ----
    fig, ax = plt.subplots(figsize=(9.5, 5))
    for i, vs in enumerate(VSIZES):
        ys = [peaks_by_vs[vs][wl][0] for wl in WORKLOADS]
        Ns_used = [peaks_by_vs[vs][wl][1] for wl in WORKLOADS]
        offsets = [p + (i - 1) * width for p in x_pos]
        ax.bar(offsets, ys, width=width, label=f"kv={vs} B", color=vs_color[vs])
        for off, y, n_used in zip(offsets, ys, Ns_used):
            ax.text(off, y + 0.4, f"{y:.1f}\nN={n_used}",
                    ha="center", va="bottom", fontsize=7)
    ax.axhline(20, linestyle="--", color=COLORS["target"], linewidth=1,
               label="target 20 Mops/s")
    ax.set_xticks(x_pos); ax.set_xticklabels(WL_LABEL)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("best-of-N peak trans_agg_thpt (Mops/s, cache=on)")
    ax.set_title("iter-5 — peak throughput per workload, best across N ∈ {1, 2, 4}")
    ax.legend(fontsize=8, loc="upper right")
    bar_with_headroom(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "iter5_bestn_thpt_bars.png"))
    plt.close(fig)

    # ---- Figure 2: latency at the throughput-best (vs, N) cell ----
    fig, ax = plt.subplots(figsize=(9.5, 5))
    for i, vs in enumerate(VSIZES):
        ys = []
        Ns_used = []
        for wl in WORKLOADS:
            thpt, N_used, T, wp50, rp50 = peaks_by_vs[vs][wl]
            p50 = rp50 if wl in USE_READ_P50 else wp50
            ys.append(p50); Ns_used.append(N_used)
        offsets = [p + (i - 1) * width for p in x_pos]
        ax.bar(offsets, ys, width=width, label=f"kv={vs} B", color=vs_color[vs])
        for off, y, n_used in zip(offsets, ys, Ns_used):
            if y > 0:
                ax.text(off, y + 0.15, f"{y:.1f}\nN={n_used}",
                        ha="center", va="bottom", fontsize=7)
    ax.set_xticks(x_pos); ax.set_xticklabels(WL_LABEL)
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("p50 latency at peak-throughput cell (us)")
    ax.set_title("iter-5 — p50 latency at best-N peak (cache=on)\n"
                 "C/D = read p50, A/B/F = write p50; N annotation = which flusher count won")
    ax.legend(fontsize=8, loc="upper left")
    bar_with_headroom(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "iter5_bestn_p50_bars.png"))
    plt.close(fig)

    # Print underlying numbers.
    print(f"{'WL':>3} | " + " | ".join(f"  kv={vs:>4}              " for vs in VSIZES))
    print("-" * 88)
    for wl, lbl in zip(WORKLOADS, WL_LABEL):
        cells = []
        for vs in VSIZES:
            thpt, N_used, T, wp50, rp50 = peaks_by_vs[vs][wl]
            p50 = rp50 if wl in USE_READ_P50 else wp50
            cells.append(f"{thpt:5.2f}@N{N_used}/T{T:<2} p50={p50:5.1f}us")
        print(f"  {lbl} | " + " | ".join(cells))

if __name__ == "__main__":
    main()
