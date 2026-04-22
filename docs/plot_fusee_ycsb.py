#!/usr/bin/env python3
"""Plot FUSEE YCSB sweep logs produced by tests/run_fusee_ycsb_sweep.sh.

Accepts one or more log files; produces a grouped bar chart per log.
- Workloads (workloada..workloadf) on x-axis.
- Three bars per workload (A, B, C).
- Paired cache-off vs cache-on if the log has both.

Usage:
  python3 docs/plot_fusee_ycsb.py <log1> [log2 ...]

Writes <logbasename>.png next to each log.
"""
import sys
import os
import re
import collections

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.stderr.write("matplotlib required (pip install matplotlib)\n")
    sys.exit(1)

# Parser: header lines are:
#   --- workload=<name> opt=<X> cache=[FUSEE_CACHE=1|] load=... trans=... ---
# Result lines are:
#   YCSB opt=<X> cache=<0|1> load_ops=N load_thpt=Z trans_ops=M trans_thpt=W
# (or trans_agg_thpt for role-mode)

HEADER = re.compile(
    r"---\s+workload=(?P<wl>\S+)\s+opt=(?P<opt>[ABC])\s+cache=\[(?P<ce>[^\]]*)\]")
RESULT = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01]).*?"
    r"trans_(?:thpt|agg_thpt)=(?P<thpt>[0-9.]+)")

def parse(path):
    # runs keyed by (workload, opt, cache_int)
    runs = {}
    cur_wl = None
    cur_opt = None
    cur_cache = None
    with open(path) as fh:
        for line in fh:
            m = HEADER.search(line)
            if m:
                cur_wl = m.group("wl")
                cur_opt = m.group("opt")
                cur_cache = 1 if "FUSEE_CACHE" in m.group("ce") else 0
                continue
            m = RESULT.search(line)
            if m:
                opt = m.group("opt")
                cache = int(m.group("c"))
                # Prefer header-derived workload name; fall back to unknown.
                wl = cur_wl or "unknown"
                runs[(wl, opt, cache)] = float(m.group("thpt"))
    return runs

def plot(path, runs):
    # Build sorted axes.
    workloads = sorted({wl for (wl, _, _) in runs.keys()})
    opts = ["A", "B", "C"]
    caches_in_log = sorted({c for (_, _, c) in runs.keys()})

    # Env filter: FUSEE_YCSB_CACHE_FILTER = "on" | "off" | "" (any)
    cache_filter = os.environ.get("FUSEE_YCSB_CACHE_FILTER", "").lower()
    if cache_filter == "on":
        caches_in_log = [c for c in caches_in_log if c == 1]
    elif cache_filter == "off":
        caches_in_log = [c for c in caches_in_log if c == 0]

    have_off = 0 in caches_in_log
    have_on = 1 in caches_in_log

    n_groups = len(workloads)
    n_bars_per_group = len(opts) * (len(caches_in_log))
    bar_w = 0.8 / n_bars_per_group

    fig, ax = plt.subplots(figsize=(1.5 * n_groups + 3, 5))

    colors_off = {"A": "#9ecae1", "B": "#fdae6b", "C": "#a1d99b"}
    colors_on  = {"A": "#3182bd", "B": "#e6550d", "C": "#31a354"}
    hatch_off = ""
    hatch_on  = "///"

    x = list(range(n_groups))
    for bi, opt in enumerate(opts):
        caches = []
        if have_off: caches.append(0)
        if have_on:  caches.append(1)
        for ci, cache in enumerate(caches):
            ys = [runs.get((wl, opt, cache), 0.0) / 1e3 for wl in workloads]
            offset = (bi * len(caches) + ci) * bar_w - 0.4 + bar_w / 2
            label = f"{opt} cache={'on' if cache else 'off'}"
            color = colors_on[opt] if cache else colors_off[opt]
            hatch = hatch_on if cache else hatch_off
            ax.bar([xi + offset for xi in x], ys, bar_w,
                   color=color, hatch=hatch, edgecolor="black", linewidth=0.3,
                   label=label)

    ax.set_xticks(x)
    ax.set_xticklabels(workloads, rotation=0)
    ax.set_ylabel("trans phase throughput (kops/s)")
    title_tag = os.path.basename(path)
    ax.set_title(f"YCSB trans throughput per workload\n{title_tag}")
    ax.legend(loc="best", fontsize=8, ncol=2)
    ax.grid(True, axis="y", alpha=0.3)

    out = os.path.splitext(path)[0] + ".png"
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")

def main(args):
    if not args:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    for path in args:
        runs = parse(path)
        if not runs:
            print(f"no results parsed from {path}", file=sys.stderr)
            continue
        plot(path, runs)

if __name__ == "__main__":
    main(sys.argv[1:])
