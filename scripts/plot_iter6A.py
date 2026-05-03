#!/usr/bin/env python3
"""iter-5A plot generator — minimal spec §6 fulfillment.

Generates per-workload throughput line plots (Mops/s vs T) with
the 20 Mops/s target line overlaid. One plot per (workload, KV size)
under outdir/A_thpt_<wl>_kv<sz>.png. Style B per docs/tools/plot_style.py.
"""
import os
import re
import sys
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LINE = re.compile(
    r"YCSB opt=A cache=(\d+) num_hosts=(\d+) threads=(\d+) threads_eff=\d+ "
    r"rep=(\d+) load_ops=\d+ load_thpt=\d+ "
    r"trans_ops=\d+ trans_wall_max=[\d\.]+ trans_agg_thpt=(\d+) "
    r"w_avg_ns=\d+ w_p50_ns=\d+ w_p99_ns=\d+ "
    r"r_avg_ns=\d+ r_p50_ns=\d+ r_p99_ns=\d+ "
    r"# (\w+)_optA_t\d+_cache(on|off)_rep\d+(?:_kv(\d+))?$"
)

TARGET_MOPS = 20.0
T_VALUES = [1, 2, 4, 8, 16, 32, 64]


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("FAIL"):
                continue
            m = LINE.match(line)
            if not m: continue
            cache, _, T, rep, agg, wl, cache_str, kv = m.groups()
            rows.append({
                "wl": wl, "T": int(T), "cache": cache_str,
                "kv": int(kv) if kv else 0,
                "agg_mops": int(agg) / 1e6,
            })
    return rows


def plot_thpt(rows, outdir, target=TARGET_MOPS):
    by_key = {}
    for r in rows:
        if r["cache"] != "on": continue
        key = (r["wl"], r["kv"])
        by_key.setdefault(key, []).append(r)
    for (wl, kv), runs in sorted(by_key.items()):
        runs.sort(key=lambda r: r["T"])
        Ts = [r["T"] for r in runs]
        Ys = [r["agg_mops"] for r in runs]
        fig, ax = plt.subplots(figsize=(6, 4))
        ax.plot(Ts, Ys, "-o", color="#333333", linewidth=2,
                markersize=6, label=f"Protocol A (KV={kv})")
        ax.axhline(y=target, color="#c44e52", linestyle="--",
                   linewidth=1.5, label=f"target {target} Mops/s")
        ax.set_xscale("log", base=2)
        ax.set_xticks(Ts)
        ax.set_xticklabels([str(t) for t in Ts])
        ax.set_xlabel("# clients per host")
        ax.set_ylabel("trans throughput (Mops/s)")
        ax.set_title(f"{wl} — Protocol A — KV={kv} B (cache=on)", pad=10)
        ax.legend(loc="upper left")
        ax.set_ylim(0, max(target, max(Ys)) * 1.25)
        ax.grid(alpha=0.3)
        out = outdir / f"A_thpt_{wl}_kv{kv}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=110)
        plt.close(fig)
        print(f"wrote {out}")


def plot_kv_compare(rows, outdir):
    by_wl = {}
    for r in rows:
        if r["cache"] != "on": continue
        by_wl.setdefault(r["wl"], []).append(r)
    for wl, runs in sorted(by_wl.items()):
        fig, ax = plt.subplots(figsize=(6, 4))
        for kv in [256, 512, 1024]:
            sub = [r for r in runs if r["kv"] == kv]
            sub.sort(key=lambda r: r["T"])
            if not sub: continue
            Ts = [r["T"] for r in sub]
            Ys = [r["agg_mops"] for r in sub]
            ax.plot(Ts, Ys, "-o", linewidth=1.6, markersize=5,
                    label=f"KV={kv}")
        ax.axhline(y=TARGET_MOPS, color="#c44e52", linestyle="--",
                   linewidth=1.5, label="target 20 Mops/s")
        ax.set_xscale("log", base=2)
        ax.set_xticks(T_VALUES)
        ax.set_xticklabels([str(t) for t in T_VALUES])
        ax.set_xlabel("# clients per host")
        ax.set_ylabel("trans throughput (Mops/s)")
        ax.set_title(f"{wl} — KV size compare (cache=on)", pad=10)
        ax.legend(loc="upper left")
        ax.grid(alpha=0.3)
        out = outdir / "extra" / f"A_kv_size_compare_{wl}.png"
        out.parent.mkdir(exist_ok=True)
        fig.tight_layout()
        fig.savefig(out, dpi=110)
        plt.close(fig)
        print(f"wrote {out}")


def main():
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    rows = parse(outdir / "SUMMARY.log")
    if not rows:
        print("no parsable rows", file=sys.stderr); sys.exit(1)
    plot_thpt(rows, outdir)
    plot_kv_compare(rows, outdir)


if __name__ == "__main__":
    main()
