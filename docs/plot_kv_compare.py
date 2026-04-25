#!/usr/bin/env python3
"""Cross-value-size comparison plot for iter-4.

Reads several SUMMARY.log files (one per FUSEE_VALUE_SIZE), and emits:
  - C_kv_compare_thpt_<wl>.png  for each workload: lines = value_size,
    x = #clients per host (log2), y = trans_agg_thpt Mops/s
  - C_kv_compare_p99_<wl>.png   same shape, y = w_p99 us
  - C_kv_compare_peak_summary.png  per-workload bars: peak thpt vs vsize

Usage:
  python3 plot_kv_compare.py <out_dir> <label1>=<sum1> [<label2>=<sum2> ...]

The labels are short tags shown in the legend (e.g. "kv8", "kv256",
"iter3-baseline", etc.). The comparison plot is honest about
"reference only" workloads C/D.
"""
import argparse, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"(?:value_size=(?P<vs>\d+)\s+)?"
    r"num_hosts=\d+\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=\d+\s+trans_wall_max=[\d.]+s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)\s+"
    r"w_avg_ns=\d+\s+w_p50_ns=\d+\s+w_p99_ns=(?P<wp99>\d+)\s+"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)_opt[ABC]_t\d+_cache(?P<c>on|off)")

WORKLOADS = ["workloada", "workloadb", "workloadc", "workloadd", "workloadf"]

def parse_c(summary_path, want_cache=1):
    out = {}
    if not os.path.exists(summary_path):
        return out
    with open(summary_path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m or m.group("opt") != "C" or int(m.group("c")) != want_cache:
                continue
            t = TAG.search(line)
            if not t: continue
            wl = t.group("wl"); T = int(m.group("T"))
            out[(wl, T)] = (float(m.group("thpt")) / 1e6,
                            float(m.group("wp99")) / 1000.0)
    return out

def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr); sys.exit(2)
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    iters = []
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print(f"bad arg {arg!r}", file=sys.stderr); sys.exit(2)
        label, path = arg.split("=", 1)
        iters.append((label, parse_c(path)))

    Ts = [1, 2, 4, 8, 16, 32, 64, 86]

    # Per-workload throughput line plots.
    for wl in WORKLOADS:
        fig, ax = plt.subplots(figsize=(7, 4))
        for label, data in iters:
            xs, ys = [], []
            for T in Ts:
                if (wl, T) in data:
                    xs.append(T); ys.append(data[(wl, T)][0])
            if xs: ax.plot(xs, ys, marker="o", label=label)
        ax.set_xscale("log", base=2)
        ax.set_xticks(Ts); ax.set_xticklabels([str(x) for x in Ts])
        ax.axhline(20, linestyle="--", color="gray", label="target 20 Mops/s")
        ax.set_xlabel("#clients per host (T)")
        ax.set_ylabel("trans_agg_thpt (Mops/s)")
        ax.set_title(f"C — {wl} — value-size compare (cache=on)")
        ax.legend(fontsize=8); ax.grid(alpha=0.3); fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_kv_compare_thpt_{wl}.png"), dpi=100)
        plt.close(fig)

    # Per-workload write p99 line plots.
    for wl in WORKLOADS:
        fig, ax = plt.subplots(figsize=(7, 4))
        for label, data in iters:
            xs, ys = [], []
            for T in Ts:
                if (wl, T) in data:
                    xs.append(T); ys.append(data[(wl, T)][1])
            if xs: ax.plot(xs, ys, marker="o", label=label)
        ax.set_xscale("log", base=2)
        ax.set_xticks(Ts); ax.set_xticklabels([str(x) for x in Ts])
        ax.set_xlabel("#clients per host (T)")
        ax.set_ylabel("write p99 (us)")
        ax.set_title(f"C — {wl} — write p99 vs value size (cache=on)")
        ax.legend(fontsize=8); ax.grid(alpha=0.3); fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_kv_compare_p99_{wl}.png"), dpi=100)
        plt.close(fig)

    # Peak summary bar chart.
    fig, ax = plt.subplots(figsize=(9, 5))
    width = 0.18
    x_pos = list(range(len(WORKLOADS)))
    for i, (label, data) in enumerate(iters):
        peaks = []
        for wl in WORKLOADS:
            best = 0.0
            for T in Ts:
                if (wl, T) in data:
                    v = data[(wl, T)][0]
                    if v > best: best = v
            peaks.append(best)
        offsets = [p + (i - (len(iters)-1)/2) * width for p in x_pos]
        ax.bar(offsets, peaks, width=width, label=label)
    ax.axhline(20, linestyle="--", color="gray", label="target 20 Mops/s")
    ax.set_xticks(x_pos); ax.set_xticklabels(WORKLOADS, rotation=15)
    ax.set_ylabel("peak trans_agg_thpt (Mops/s, cache=on)")
    ax.set_title("C — peak throughput vs value size (per workload)")
    ax.legend(fontsize=8); ax.grid(axis="y", alpha=0.3); fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "C_kv_compare_peak_summary.png"), dpi=100)
    plt.close(fig)

if __name__ == "__main__":
    main()
