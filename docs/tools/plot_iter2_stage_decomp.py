#!/usr/bin/env python3
"""Plot iter-2 write-path stage decomposition (workload A cache=on).

Reads a latency-decomp SUMMARY.log (format produced by
tests/cxl_latency_decomp_C.cc / scripts/run_latency_decomp_C.sh) and
emits a two-panel figure: avg-stacked bars on the left, p99-stacked
bars on the right, one column per T value.

Usage:
  python3 plot_iter2_stage_decomp.py <decomp_summary.log> <out.png>
"""
import re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LINE = re.compile(
    r"^DECOMP_C\s+threads=(?P<T>\d+).*?"
    r"stage_lock_avg=(?P<la>\d+)\s+stage_lock_p50=(?P<l5>\d+)\s+stage_lock_p99=(?P<l9>\d+)\s+"
    r"stage_scan_avg=(?P<sa>\d+)\s+stage_scan_p50=\d+\s+stage_scan_p99=(?P<s9>\d+)\s+"
    r"stage_publish_avg=(?P<pa>\d+)\s+stage_publish_p50=\d+\s+stage_publish_p99=(?P<p9>\d+)\s+"
    r"stage_epoch_avg=(?P<ea>\d+)\s+stage_epoch_p50=\d+\s+stage_epoch_p99=(?P<e9>\d+)\s+"
    r"stage_unlock_avg=(?P<ua>\d+)\s+stage_unlock_p50=\d+\s+stage_unlock_p99=(?P<u9>\d+)\s+"
    r"stage_total_avg=(?P<ta>\d+)\s+stage_total_p50=\d+\s+stage_total_p99=(?P<t9>\d+)"
)

STAGES = ["lock", "scan", "publish", "epoch", "unlock"]
COLORS = {"lock": "#d95f02", "scan": "#1b9e77", "publish": "#7570b3",
          "epoch": "#e7298a", "unlock": "#66a61e"}

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            m = LINE.search(line)
            if not m: continue
            T = int(m.group("T"))
            to_us = lambda ns: float(ns) / 1000.0
            rows.append({
                "T": T,
                "avg": {
                    "lock":    to_us(m.group("la")),
                    "scan":    to_us(m.group("sa")),
                    "publish": to_us(m.group("pa")),
                    "epoch":   to_us(m.group("ea")),
                    "unlock":  to_us(m.group("ua")),
                    "total":   to_us(m.group("ta")),
                },
                "p99": {
                    "lock":    to_us(m.group("l9")),
                    "scan":    to_us(m.group("s9")),
                    "publish": to_us(m.group("p9")),
                    "epoch":   to_us(m.group("e9")),
                    "unlock":  to_us(m.group("u9")),
                    "total":   to_us(m.group("t9")),
                },
            })
    rows.sort(key=lambda r: r["T"])
    return rows

def draw_panel(ax, rows, which, title):
    Ts = [r["T"] for r in rows]
    xpos = np.arange(len(Ts))
    bottom = np.zeros(len(Ts))
    for st in STAGES:
        vals = np.array([r[which][st] for r in rows])
        ax.bar(xpos, vals, bottom=bottom, color=COLORS[st], label=st,
               edgecolor="white", linewidth=0.6)
        bottom += vals
    # Annotate total on top
    totals = [r[which]["total"] for r in rows]
    for i, tot in enumerate(totals):
        ax.text(xpos[i], bottom[i], f" {tot:.1f} µs", ha="center",
                va="bottom", fontsize=8)
    ax.set_xticks(xpos)
    ax.set_xticklabels([f"T={t}" for t in Ts])
    ax.set_ylabel("latency (µs)")
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.3)

def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr); sys.exit(2)
    summary, out = sys.argv[1], sys.argv[2]
    rows = parse(summary)
    if not rows:
        print("no DECOMP_C rows parsed", file=sys.stderr); sys.exit(1)

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    draw_panel(axes[0], rows, "avg",
               "iter-2 C write-path stages (workload A cache=on, avg)")
    draw_panel(axes[1], rows, "p99",
               "iter-2 C write-path stages (workload A cache=on, p99)")
    axes[1].set_yscale("log")
    axes[0].legend(loc="upper left", fontsize=9)
    fig.suptitle("Per-slot LFM + bump_epoch-outside-crit — stage decomp",
                 fontsize=11)
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
