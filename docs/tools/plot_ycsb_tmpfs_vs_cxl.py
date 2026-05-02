#!/usr/bin/env python3
"""Compare YCSB trans throughput on tmpfs vs real CXL, cache-on only.

Reads two logs (tmpfs + real-CXL emr) and produces a grouped bar chart:
  6 workloads × 3 protocols (A/B/C) × 2 backings (tmpfs / CXL).

Usage:
  python3 docs/plot_ycsb_tmpfs_vs_cxl.py <tmpfs.log> <cxl.log> [out.png]
"""
import sys, re, os
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib required")

HEADER = re.compile(
    r"---\s+workload=(?P<wl>\S+)\s+opt=(?P<opt>[ABC])\s+cache=\[(?P<ce>[^\]]*)\]")
RESULT = re.compile(
    r"^YCSB\s+opt=(?P<opt>[ABC])\s+cache=(?P<c>[01]).*?"
    r"trans_(?:thpt|agg_thpt)=(?P<thpt>[0-9.]+)")

def parse(path, want_cache=1):
    runs = {}
    cur_wl = cur_opt = cur_cache = None
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
                c = int(m.group("c"))
                if c == want_cache:
                    runs[(cur_wl, opt)] = float(m.group("thpt"))
    return runs

def main(args):
    if len(args) < 2:
        sys.exit("usage: plot_ycsb_tmpfs_vs_cxl.py <tmpfs.log> <cxl.log> [out.png]")
    tmpfs_path, cxl_path = args[0], args[1]
    out = args[2] if len(args) > 2 else "docs/fusee_ycsb_tmpfs_vs_cxl_cache_on.png"
    tmpfs = parse(tmpfs_path, want_cache=1)
    cxl   = parse(cxl_path, want_cache=1)
    wls = sorted({wl for (wl, _) in tmpfs.keys()} | {wl for (wl, _) in cxl.keys()})
    opts = ["A", "B", "C"]

    n_groups = len(wls)
    bar_w = 0.8 / (len(opts) * 2)
    fig, ax = plt.subplots(figsize=(1.4 * n_groups + 4, 5))

    base_colors = {"A": "#3182bd", "B": "#e6550d", "C": "#31a354"}
    x = list(range(n_groups))

    for bi, opt in enumerate(opts):
        for ci, (label, src) in enumerate([("tmpfs", tmpfs), ("CXL", cxl)]):
            ys = [src.get((wl, opt), 0.0) / 1e3 for wl in wls]
            offset = (bi * 2 + ci) * bar_w - 0.4 + bar_w / 2
            color = base_colors[opt]
            alpha = 0.45 if label == "tmpfs" else 1.0
            hatch = "//" if label == "tmpfs" else ""
            ax.bar([xi + offset for xi in x], ys, bar_w,
                   color=color, alpha=alpha, hatch=hatch,
                   edgecolor="black", linewidth=0.3,
                   label=f"{opt} {label}")

    ax.set_xticks(x)
    ax.set_xticklabels(wls, rotation=0)
    ax.set_ylabel("trans phase throughput (kops/s), cache=on")
    ax.set_title("YCSB trans throughput: tmpfs baseline vs real CXL (emr /dev/dax0.0)\n"
                 "cache-on path; single-host, 200 k ops/phase, FUSEE port")
    ax.legend(loc="best", fontsize=8, ncol=3)
    ax.grid(True, axis="y", alpha=0.3)

    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")

if __name__ == "__main__":
    main(sys.argv[1:])
