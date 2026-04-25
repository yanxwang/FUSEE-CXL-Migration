#!/usr/bin/env python3
"""Plot g3+g4 scaling sweep results.

Reads a SUMMARY.log produced by scripts/run_g34_scaling_sweep.sh and emits
30 primary plots under an output directory:

  {A,B,C} x thpt x {a,b,c,d,f}   -- 15 throughput plots
  {A,B,C} x lat  x {a,b,c,d,f}   -- 15 latency plots (bar × 3: avg/p50/p99)

Latency plots are emitted as:
  <opt>_lat_<wl>_write.png  (always)
  <opt>_lat_<wl>_read.png   (if this workload has any reads)

So: 15 thpt + 15 write-lat + (up to 15) read-lat = up to 45 files per
cache mode. We run with --cache=on by default (the primary 30-plot deck)
and separately for --cache=off (supplementary).

Usage:
  python3 plot_scaling_sweep.py <summary.log> <out_dir> [--cache=on|off]
"""
import argparse, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

YCSB = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01])\s+"
    r"(?:value_size=(?P<vs>\d+)\s+)?"
    r"(?:num_flushers=(?P<nf>\d+)\s+)?"
    r"num_hosts=(?P<H>\d+)\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=(?P<tops>\d+)\s+trans_wall_max=(?P<twall>[\d.]+)s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)\s+"
    r"w_avg_ns=(?P<wavg>\d+)\s+w_p50_ns=(?P<wp50>\d+)\s+w_p99_ns=(?P<wp99>\d+)\s+"
    r"r_avg_ns=(?P<ravg>\d+)\s+r_p50_ns=(?P<rp50>\d+)\s+r_p99_ns=(?P<rp99>\d+)"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)_opt(?P<opt>[ABC])_t(?P<T>\d+)_cache(?P<c>on|off)")

def parse(log_path, want_cache="on"):
    want = 1 if want_cache == "on" else 0
    # Each useful line is a YCSB line ALSO tagged with workload. Earlier log
    # format appends "  # <tag>" so we can grep the workload from the tag.
    runs = {}  # (opt, wl, T) -> dict of numeric stats
    with open(log_path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m: continue
            if int(m.group("c")) != want: continue
            t = TAG.search(line)
            wl = t.group("wl") if t else "unknown"
            opt = m.group("opt"); T = int(m.group("T"))
            runs[(opt, wl, T)] = {
                "thpt": float(m.group("thpt")),
                "w_avg": float(m.group("wavg")) / 1000.0,  # -> us
                "w_p50": float(m.group("wp50")) / 1000.0,
                "w_p99": float(m.group("wp99")) / 1000.0,
                "r_avg": float(m.group("ravg")) / 1000.0,
                "r_p50": float(m.group("rp50")) / 1000.0,
                "r_p99": float(m.group("rp99")) / 1000.0,
            }
    return runs

def plot_thpt(opt, wl, Ts, thpts, out_path):
    # Default per scaling_ycsb_spec.md: linear y, Mops/s. Log-y branch kept
    # commented for regen.
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ys = [t / 1e6 for t in thpts]  # Mops/s
    ax.plot(Ts, ys, "o-", lw=2, markersize=7,
            color={"A":"#3182bd","B":"#e6550d","C":"#31a354"}[opt])
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ts); ax.set_xticklabels([str(t) for t in Ts])
    # LOG-Y (commented; uncomment to re-enable):
    #   ax.set_yscale("log")
    ax.set_xlabel("#clients per host")
    ax.set_ylabel("Throughput (Mops/s)")
    ax.set_title(f"Option {opt} — {wl} — throughput vs #clients/host\n"
                 f"(g3+g4 cross-host, role-mode, cache on)")
    ax.grid(True, alpha=0.3)
    if ys:
        best = max(ys)
        best_t = Ts[ys.index(best)]
        ax.annotate(f"peak {best:.1f} Mops/s @ T={best_t}",
                    xy=(best_t, best),
                    xytext=(5, 5), textcoords="offset points", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)

def plot_lat(opt, wl, Ts, series_avg, series_p50, series_p99, out_path,
             which):
    """which in {'write','read'}: pick color; title.
    Default per scaling_ycsb_spec.md: linear y. Log-y branch kept commented."""
    fig, ax = plt.subplots(figsize=(8, 4.5))
    n_ts = len(Ts)
    x = np.arange(n_ts)
    w = 0.27
    ax.bar(x - w, series_avg, w, label="avg", color="#6baed6")
    ax.bar(x,     series_p50, w, label="p50", color="#fd8d3c")
    ax.bar(x + w, series_p99, w, label="p99", color="#74c476")
    # LOG-Y (commented; uncomment to re-enable):
    #   def sanitize(vs): return [max(v, 0.01) for v in vs]
    #   ax.bar(x - w, sanitize(series_avg), ...)
    #   ax.set_yscale("log")
    ax.set_xticks(x); ax.set_xticklabels([str(t) for t in Ts])
    ax.set_xlabel("#clients per host")
    ax.set_ylabel(f"{which} latency per op (μs)")
    ax.set_title(f"Option {opt} — {wl} — {which} latency vs #clients/host\n"
                 f"(bars: avg / p50 / p99)")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    for i, v in enumerate(series_p99):
        if v > 0:
            ax.text(x[i] + w, v, f"{v:.0f}", ha="center",
                    va="bottom", fontsize=7)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log_path")
    ap.add_argument("out_dir")
    ap.add_argument("--cache", choices=["on", "off"], default="on")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    runs = parse(args.log_path, want_cache=args.cache)
    if not runs:
        sys.exit("no matching runs parsed")

    opts = sorted({k[0] for k in runs})
    wls  = sorted({k[1] for k in runs})

    # Thread axis: take union and sort by value
    Ts_all = sorted({k[2] for k in runs})

    count = 0
    for opt in opts:
        for wl in wls:
            # ensure ordered T series
            thpts = []; wa = []; wp50 = []; wp99 = []
            ra = []; rp50 = []; rp99 = []
            Ts = []
            for T in Ts_all:
                if (opt, wl, T) in runs:
                    d = runs[(opt, wl, T)]
                    Ts.append(T)
                    thpts.append(d["thpt"])
                    wa.append(d["w_avg"]); wp50.append(d["w_p50"]); wp99.append(d["w_p99"])
                    ra.append(d["r_avg"]); rp50.append(d["r_p50"]); rp99.append(d["r_p99"])
            if not Ts:
                continue

            # Throughput
            plot_thpt(opt, wl, Ts, thpts,
                      os.path.join(args.out_dir, f"{opt}_thpt_{wl}.png"))
            count += 1
            # Write latency (almost always present)
            if any(v > 0 for v in wa):
                plot_lat(opt, wl, Ts, wa, wp50, wp99,
                         os.path.join(args.out_dir, f"{opt}_lat_{wl}_write.png"),
                         "write")
                count += 1
            # Read latency (may be absent for pure-write workloads)
            if any(v > 0 for v in ra):
                plot_lat(opt, wl, Ts, ra, rp50, rp99,
                         os.path.join(args.out_dir, f"{opt}_lat_{wl}_read.png"),
                         "read")
                count += 1

    print(f"wrote {count} plots under {args.out_dir}")

if __name__ == "__main__":
    main()
