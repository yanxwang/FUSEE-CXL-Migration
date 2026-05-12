#!/usr/bin/env python3
"""iter-11A plot generator — scaling_ycsb_spec §6 fulfillment.

Reads <out>/SUMMARY.log produced by iter11A_sweep.sh and emits:
  - A_thpt_<wl>_kv<sz>.png    (one per workload × KV, cache=on by default)
  - A_thpt_<wl>_kv<sz>_off.png (cache=off variant)
  - extra/A_kv_size_compare_<wl>.png (KV-size overlay per workload)
  - extra/A_target_<wl>_kv<sz>.png   (throughput vs 20 Mops/s bar gap)
  - extra/A_scaling_efficiency.png   (peak/single-T ratio per workload)

Line format includes first_op_ns_* fields added in iter-10A/iter-11A.
"""
import os
import re
import sys
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LINE = re.compile(
    r"YCSB opt=A cache=(\d+) num_hosts=\d+ threads=(\d+) threads_eff=\d+ "
    r"rep=\d+ load_ops=\d+ load_thpt=\d+ "
    r"trans_ops=\d+ trans_wall_max=[\d\.]+ trans_agg_thpt=(\d+) "
    r"w_avg_ns=\d+ w_p50_ns=\d+ w_p99_ns=(\d+) "
    r"r_avg_ns=\d+ r_p50_ns=\d+ r_p99_ns=(\d+).*"
    r"# (\w+)_optA_t\d+_cache(on|off)_rep\d+_kv(\d+)$"
)

TARGET_MOPS = 20.0
T_VALUES = [1, 2, 4, 8, 16, 32, 64]


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("FAIL") or not line:
                continue
            m = LINE.match(line)
            if not m:
                continue
            cache_int, T, agg, w_p99, r_p99, wl, cache_str, kv = m.groups()
            rows.append({
                "wl": wl, "T": int(T), "cache": cache_str,
                "kv": int(kv),
                "agg_mops": int(agg) / 1e6,
                "w_p99_us": int(w_p99) / 1000.0,
                "r_p99_us": int(r_p99) / 1000.0,
            })
    return rows


def plot_thpt(rows, outdir, cache="on"):
    by_key = {}
    for r in rows:
        if r["cache"] != cache:
            continue
        key = (r["wl"], r["kv"])
        by_key.setdefault(key, []).append(r)
    for (wl, kv), runs in sorted(by_key.items()):
        runs.sort(key=lambda r: r["T"])
        Ts = [r["T"] for r in runs]
        Ys = [r["agg_mops"] for r in runs]
        fig, ax = plt.subplots(figsize=(6, 4))
        ax.plot(Ts, Ys, "-o", color="#333333", linewidth=2,
                markersize=6, label=f"Protocol A (KV={kv} cache={cache})")
        ax.axhline(y=TARGET_MOPS, color="#c44e52", linestyle="--",
                   linewidth=1.5, label=f"target {TARGET_MOPS:.0f} Mops/s")
        ax.set_xscale("log", base=2)
        ax.set_xticks(Ts if Ts else T_VALUES)
        ax.set_xticklabels([str(t) for t in (Ts if Ts else T_VALUES)])
        ax.set_xlabel("# clients per host")
        ax.set_ylabel("trans throughput (Mops/s)")
        ax.set_title(f"{wl} — Protocol A — KV={kv} B (cache={cache}) [iter-11A]", pad=10)
        ax.legend(loc="upper left", fontsize=8)
        ymax = max([TARGET_MOPS] + Ys) * 1.25
        ax.set_ylim(0, ymax)
        ax.grid(alpha=0.3)
        suffix = "" if cache == "on" else "_off"
        out = outdir / f"A_thpt_{wl}_kv{kv}{suffix}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=110)
        plt.close(fig)
        print(f"wrote {out}")


def plot_kv_compare(rows, outdir, cache="on"):
    by_wl = {}
    for r in rows:
        if r["cache"] != cache:
            continue
        by_wl.setdefault(r["wl"], []).append(r)
    extra = outdir / "extra"
    extra.mkdir(exist_ok=True)
    for wl, runs in sorted(by_wl.items()):
        fig, ax = plt.subplots(figsize=(6, 4))
        plotted_any = False
        for kv, color in zip([256, 512, 1024],
                             ["#4878d0", "#ee854a", "#6acc64"]):
            sub = [r for r in runs if r["kv"] == kv]
            sub.sort(key=lambda r: r["T"])
            if not sub:
                continue
            Ts = [r["T"] for r in sub]
            Ys = [r["agg_mops"] for r in sub]
            ax.plot(Ts, Ys, "-o", linewidth=1.6, markersize=5,
                    label=f"KV={kv}", color=color)
            plotted_any = True
        if not plotted_any:
            plt.close(fig); continue
        ax.axhline(y=TARGET_MOPS, color="#c44e52", linestyle="--",
                   linewidth=1.5, label=f"target {TARGET_MOPS:.0f} Mops/s")
        ax.set_xscale("log", base=2)
        ax.set_xticks(T_VALUES)
        ax.set_xticklabels([str(t) for t in T_VALUES])
        ax.set_xlabel("# clients per host")
        ax.set_ylabel("trans throughput (Mops/s)")
        ax.set_title(f"{wl} — KV size compare (cache={cache}) [iter-11A]", pad=10)
        ax.legend(loc="upper left", fontsize=8)
        ax.grid(alpha=0.3)
        out = extra / f"A_kv_size_compare_{wl}_{cache}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=110)
        plt.close(fig)
        print(f"wrote {out}")


def plot_target_gap(rows, outdir):
    """Per workload bar chart: best-cell Mops/s vs 20 target."""
    by_wl = {}
    for r in rows:
        by_wl.setdefault(r["wl"], []).append(r)
    extra = outdir / "extra"
    extra.mkdir(exist_ok=True)
    wls = sorted(by_wl.keys())
    bests = []
    cells = []
    for wl in wls:
        best = max(by_wl[wl], key=lambda r: r["agg_mops"])
        bests.append(best["agg_mops"])
        cells.append(f"T={best['T']}\n{best['cache']} kv={best['kv']}")
    fig, ax = plt.subplots(figsize=(7, 4))
    bars = ax.bar(wls, bests, color="#4878d0",
                  edgecolor="#333333", linewidth=0.5)
    ax.axhline(y=TARGET_MOPS, color="#c44e52", linestyle="--",
               linewidth=1.5, label=f"target {TARGET_MOPS:.0f} Mops/s")
    for bar, value, cell in zip(bars, bests, cells):
        ax.text(bar.get_x() + bar.get_width() / 2,
                value + 0.5, f"{value:.2f}\n{cell}",
                ha="center", va="bottom", fontsize=7)
    ax.set_ylabel("trans throughput (Mops/s)")
    ax.set_title("iter-11A — best-cell per workload vs 20 Mops/s target", pad=10)
    ax.set_ylim(0, TARGET_MOPS * 1.15)
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.3, axis="y")
    out = extra / "A_target_per_workload.png"
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"wrote {out}")


def plot_scaling_efficiency(rows, outdir):
    """peak-T / T=1 throughput ratio per workload (cache=on best KV)."""
    by_key = {}
    for r in rows:
        if r["cache"] != "on":
            continue
        by_key.setdefault((r["wl"], r["kv"]), []).append(r)
    extra = outdir / "extra"
    extra.mkdir(exist_ok=True)
    by_wl_best = {}
    for (wl, kv), runs in by_key.items():
        runs.sort(key=lambda r: r["T"])
        if not any(r["T"] == 1 for r in runs):
            continue
        t1 = next(r for r in runs if r["T"] == 1)["agg_mops"]
        peak = max(runs, key=lambda r: r["agg_mops"])
        ratio = peak["agg_mops"] / t1 if t1 > 0 else 0
        prev = by_wl_best.get(wl)
        if prev is None or peak["agg_mops"] > prev[1]:
            by_wl_best[wl] = (kv, peak["agg_mops"], ratio, peak["T"], t1)
    wls = sorted(by_wl_best.keys())
    ratios = [by_wl_best[w][2] for w in wls]
    labels = [f"{w}\nkv={by_wl_best[w][0]}\nT={by_wl_best[w][3]}\n{by_wl_best[w][4]:.2f}→{by_wl_best[w][1]:.2f}" for w in wls]
    fig, ax = plt.subplots(figsize=(7, 4.5))
    bars = ax.bar(wls, ratios, color="#956cb4",
                  edgecolor="#333333", linewidth=0.5)
    for bar, value, label in zip(bars, ratios, labels):
        ax.text(bar.get_x() + bar.get_width() / 2, value + 0.3,
                f"{value:.1f}×", ha="center", va="bottom", fontsize=8)
        ax.text(bar.get_x() + bar.get_width() / 2, -2, label,
                ha="center", va="top", fontsize=6)
    ax.set_ylabel("peak-T thpt / T=1 thpt (scaling efficiency)")
    ax.set_title("iter-11A — scaling efficiency (best cache=on KV per workload)", pad=10)
    ax.set_ylim(0, max(ratios) * 1.3 if ratios else 10)
    ax.grid(alpha=0.3, axis="y")
    fig.subplots_adjust(bottom=0.25)
    out = extra / "A_scaling_efficiency.png"
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"wrote {out}")


def main():
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    rows = parse(outdir / "SUMMARY.log")
    if not rows:
        print("no parsable rows", file=sys.stderr)
        sys.exit(1)
    print(f"parsed {len(rows)} rows")
    plot_thpt(rows, outdir, cache="on")
    plot_thpt(rows, outdir, cache="off")
    plot_kv_compare(rows, outdir, cache="on")
    plot_kv_compare(rows, outdir, cache="off")
    plot_target_gap(rows, outdir)
    plot_scaling_efficiency(rows, outdir)


if __name__ == "__main__":
    main()
