#!/usr/bin/env python3
"""Extra analysis plots on top of plot_scaling_sweep.py.

Produces:
  - abc_compare_<wl>.png         : A vs B vs C throughput, x=T, per workload
  - abc_compare_<wl>_lat.png     : A vs B vs C write p99 latency, x=T, per workload
  - cache_speedup_<opt>.png      : cache-on/cache-off ratio vs T, per opt, all wls
  - scaling_efficiency_C.png     : C scaling efficiency vs T (thpt(T)/thpt(1)/T)
  - summary_table.md             : best-T per (opt, wl), comparison
"""
import argparse, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"num_hosts=(?P<H>\d+)\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=(?P<tops>\d+)\s+trans_wall_max=(?P<twall>[\d.]+)s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)\s+"
    r"w_avg_ns=(?P<wavg>\d+)\s+w_p50_ns=(?P<wp50>\d+)\s+w_p99_ns=(?P<wp99>\d+)\s+"
    r"r_avg_ns=(?P<ravg>\d+)\s+r_p50_ns=(?P<rp50>\d+)\s+r_p99_ns=(?P<rp99>\d+)"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)_opt(?P<opt>[ABC])_t(?P<T>\d+)_cache(?P<c>on|off)")

def parse(log_path):
    runs = {}  # (opt, wl, T, cache) -> dict
    with open(log_path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m: continue
            t = TAG.search(line)
            wl = t.group("wl") if t else "unknown"
            key = (m.group("opt"), wl, int(m.group("T")), int(m.group("c")))
            runs[key] = {
                "thpt": float(m.group("thpt")),
                "w_avg": float(m.group("wavg")) / 1000.0,
                "w_p50": float(m.group("wp50")) / 1000.0,
                "w_p99": float(m.group("wp99")) / 1000.0,
                "r_avg": float(m.group("ravg")) / 1000.0,
                "r_p50": float(m.group("rp50")) / 1000.0,
                "r_p99": float(m.group("rp99")) / 1000.0,
            }
    return runs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out_dir")
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    runs = parse(args.log)
    opts = ["A", "B", "C"]
    colors = {"A":"#3182bd","B":"#e6550d","C":"#31a354"}
    wls = sorted({k[1] for k in runs})
    Ts_all = sorted({k[2] for k in runs})

    # 1. A/B/C throughput comparison per workload (cache=on)
    for wl in wls:
        fig, ax = plt.subplots(figsize=(7, 4.5))
        for opt in opts:
            Ts, thpts = [], []
            for T in Ts_all:
                k = (opt, wl, T, 1)
                if k in runs:
                    Ts.append(T); thpts.append(runs[k]["thpt"] / 1e3)
            if Ts:
                ax.plot(Ts, thpts, "o-", lw=2, markersize=7,
                        color=colors[opt], label=f"opt {opt}")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks([1,2,4,8,16,32,64,86])
        ax.set_xticklabels(["1","2","4","8","16","32","64","86"])
        ax.set_xlabel("#clients per host")
        ax.set_ylabel("trans agg throughput (kops/s)")
        ax.set_title(f"A/B/C comparison — {wl} — cache on\n"
                     "(A/B clamped to 1 worker/host; per-host PendingRing state)")
        ax.legend(); ax.grid(True, alpha=0.3, which="both")
        fig.tight_layout()
        fig.savefig(os.path.join(args.out_dir, f"abc_compare_{wl}.png"), dpi=150)
        plt.close(fig)

    # 2. A/B/C write p99 latency comparison per workload (cache=on)
    for wl in wls:
        fig, ax = plt.subplots(figsize=(7, 4.5))
        has_data = False
        for opt in opts:
            Ts, lats = [], []
            for T in Ts_all:
                k = (opt, wl, T, 1)
                if k in runs and runs[k]["w_p99"] > 0:
                    Ts.append(T); lats.append(runs[k]["w_p99"])
            if Ts:
                has_data = True
                ax.plot(Ts, lats, "s-", lw=2, markersize=7,
                        color=colors[opt], label=f"opt {opt}")
        if not has_data:
            plt.close(fig); continue
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks([1,2,4,8,16,32,64,86])
        ax.set_xticklabels(["1","2","4","8","16","32","64","86"])
        ax.set_xlabel("#clients per host")
        ax.set_ylabel("write p99 latency (μs, log)")
        ax.set_title(f"A/B/C write p99 — {wl} — cache on")
        ax.legend(); ax.grid(True, alpha=0.3, which="both")
        fig.tight_layout()
        fig.savefig(os.path.join(args.out_dir, f"abc_compare_{wl}_lat.png"),
                    dpi=150)
        plt.close(fig)

    # 3. Cache speedup (thpt cache_on / thpt cache_off) per opt
    for opt in opts:
        fig, ax = plt.subplots(figsize=(7, 4.5))
        wls_with_data = []
        for wl in wls:
            Ts, ratios = [], []
            for T in Ts_all:
                on = runs.get((opt, wl, T, 1))
                off = runs.get((opt, wl, T, 0))
                if on and off and off["thpt"] > 0:
                    Ts.append(T); ratios.append(on["thpt"] / off["thpt"])
            if Ts:
                wls_with_data.append(wl)
                ax.plot(Ts, ratios, "o-", lw=2, markersize=6, label=wl)
        ax.axhline(1.0, color="grey", lw=0.5, ls="--")
        ax.set_xscale("log", base=2)
        ax.set_xticks([1,2,4,8,16,32,64,86])
        ax.set_xticklabels(["1","2","4","8","16","32","64","86"])
        ax.set_xlabel("#clients per host")
        ax.set_ylabel("cache-on / cache-off throughput ratio")
        ax.set_title(f"DRAM-cache speedup — opt {opt} — per workload")
        ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(args.out_dir, f"cache_speedup_{opt}.png"), dpi=150)
        plt.close(fig)

    # 4. Scaling efficiency for C: thpt(T) / (T * thpt(T=1))
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for wl in wls:
        base_k = ("C", wl, 1, 1)
        if base_k not in runs: continue
        base = runs[base_k]["thpt"]
        Ts, effs = [], []
        for T in Ts_all:
            k = ("C", wl, T, 1)
            if k in runs and base > 0:
                Ts.append(T)
                effs.append(runs[k]["thpt"] / (T * base) * 100)
        if Ts:
            ax.plot(Ts, effs, "o-", lw=2, markersize=6, label=wl)
    ax.axhline(100, color="grey", lw=0.5, ls="--", label="ideal linear (100 %)")
    ax.set_xscale("log", base=2)
    ax.set_xticks([1,2,4,8,16,32,64,86])
    ax.set_xticklabels(["1","2","4","8","16","32","64","86"])
    ax.set_xlabel("#clients per host")
    ax.set_ylabel("scaling efficiency (%)")
    ax.set_title("Opt C scaling efficiency — thpt(T) / (T × thpt(1)) × 100\n"
                 "(cache-on; 2 hosts × T clients/host; 65536 buckets)")
    ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    ax.set_ylim(0, 130)
    fig.tight_layout()
    fig.savefig(os.path.join(args.out_dir, "scaling_efficiency_C.png"), dpi=150)
    plt.close(fig)

    # 5. Summary markdown table
    with open(os.path.join(args.out_dir, "summary_table.md"), "w") as fh:
        fh.write("# Scaling sweep summary table (cache-on)\n\n")
        fh.write("`trans_agg_thpt` (kops/s) cross both hosts; peak T per (opt, wl):\n\n")
        fh.write("| wl | opt | T=1 | T=2 | T=4 | T=8 | T=16 | T=32 | T=64 | T=86 | peak @ T |\n")
        fh.write("|---|---|---|---|---|---|---|---|---|---|---|\n")
        for wl in wls:
            for opt in opts:
                row = [wl, opt]
                pk_thpt = 0; pk_T = 0
                for T in [1,2,4,8,16,32,64,86]:
                    k = (opt, wl, T, 1)
                    if k in runs:
                        t = runs[k]["thpt"] / 1e3
                        row.append(f"{t:.0f}")
                        if t > pk_thpt: pk_thpt = t; pk_T = T
                    else:
                        row.append("—")
                row.append(f"{pk_thpt:.0f} @ T={pk_T}")
                fh.write("| " + " | ".join(row) + " |\n")

    print(f"wrote extra plots + summary_table.md under {args.out_dir}")

if __name__ == "__main__":
    main()
