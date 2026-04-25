#!/usr/bin/env python3
"""Overlay C-protocol thpt + write-p99 across multiple iterations.

Usage:
  python3 plot_c_compare.py <out_dir> <iter_label1> <summary1> [<iter_label2> <summary2> ...]

Writes:
  <out_dir>/C_compare_workload{a,b,c,d,f}.png     (throughput overlay)
  <out_dir>/C_compare_workload{a,b,d,f}_lat.png   (write-p99 overlay; c has no writes)

One subplot per workload, one line per iteration label.
"""
import os, re, sys
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
WORKLOADS_WITH_WRITES = ["workloada", "workloadb", "workloadd", "workloadf"]

def parse_c(summary_path):
    out = {}  # (wl, T) -> (thpt Mops/s, wp99 us)
    if not os.path.exists(summary_path):
        return out
    with open(summary_path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m or m.group("opt") != "C" or m.group("c") != "1":
                continue
            t = TAG.search(line)
            if not t: continue
            wl = t.group("wl")
            T = int(m.group("T"))
            out[(wl, T)] = (float(m.group("thpt")) / 1e6,
                            float(m.group("wp99")) / 1000.0)
    return out

def main():
    if len(sys.argv) < 4 or (len(sys.argv) - 2) % 2:
        print(__doc__, file=sys.stderr); sys.exit(2)
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    iters = []
    for i in range(2, len(sys.argv), 2):
        iters.append((sys.argv[i], parse_c(sys.argv[i+1])))

    Ts = [1, 2, 4, 8, 16, 32, 64, 86]

    for wl in WORKLOADS:
        fig, ax = plt.subplots(figsize=(6, 4))
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
        ax.set_title(f"C — {wl} — iteration compare")
        ax.legend(fontsize=8)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_compare_{wl}.png"), dpi=100)
        plt.close(fig)

    for wl in WORKLOADS_WITH_WRITES:
        fig, ax = plt.subplots(figsize=(6, 4))
        for label, data in iters:
            xs, ys = [], []
            for T in Ts:
                if (wl, T) in data:
                    xs.append(T); ys.append(data[(wl, T)][1])
            if xs: ax.plot(xs, ys, marker="o", label=label)
        ax.set_xscale("log", base=2)
        ax.set_xticks(Ts); ax.set_xticklabels([str(x) for x in Ts])
        ax.set_xlabel("#clients per host (T)")
        ax.set_ylabel("write p99 (μs)")
        ax.set_title(f"C — {wl} — write p99 iteration compare")
        ax.legend(fontsize=8)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_compare_{wl}_lat.png"), dpi=100)
        plt.close(fig)

if __name__ == "__main__":
    main()
