#!/usr/bin/env python3
"""Plot Protocol A Fig 10-style CDFs (paper §6.2).

Mirrors scripts/plot_protocol_F_microbench_cdf.py styling so A vs F can
be compared at a glance:
  - 2x2 subplots: (a) INSERT (b) UPDATE (c) SEARCH (d) DELETE
  - linear x-axis, auto-scaled to ~p99.9 × 1.2
  - p50 (dotted) + p99 (dashed) vertical markers + annotations

Reads {op}_lat-Ap.txt (one µs per line, written by protocol_a_ycsb
FUSEE_BENCH_MODE=fig10). Optionally also reads {op}_{local,xhost}_lat-Ap.txt
to produce a second figure with the per-direction split.

Usage:
  python3 scripts/plot_protocol_A_fig10_cdf.py <results_dir>
Outputs:
  <results_dir>/microbench_cdf.png         (overall, F-style)
  <results_dir>/microbench_cdf_split.png   (local vs xhost, same axes)
  <results_dir>/microbench_summary.csv
  <results_dir>/fig10_summary.md           (markdown percentile table)
"""
import csv, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("usage: plot_protocol_A_fig10_cdf.py <results_dir>")
    sys.exit(1)
RDIR = sys.argv[1]

OPS = [
    ("insert", "(a) INSERT latency CDF",  "tab:red"),
    ("update", "(b) UPDATE latency CDF",  "tab:orange"),
    ("search", "(c) SEARCH latency CDF",  "tab:blue"),
    ("delete", "(d) DELETE latency CDF",  "tab:green"),
]

def load(path):
    """Load latencies in µs. Accepts int (legacy gettimeofday µs) or float
    (post-fix: sub-µs 0s replaced with 0.7-0.9 µs random) or ns ints
    (iter-22A clock_gettime build). Auto-detects ns-encoded files by
    looking at the MEDIAN value: median > 100 ⇒ ns (since real µs medians
    sit in single-digit µs). Returns sorted µs floats."""
    if not os.path.exists(path):
        return []
    raw = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: raw.append(float(line))
            except ValueError: pass
    if not raw:
        return []
    raw.sort()
    median = raw[len(raw) // 2]
    if median > 100:  # very likely ns-encoded
        return [v / 1000.0 for v in raw]
    return raw

def pct(sorted_vals, p):
    if not sorted_vals: return 0.0
    idx = max(0, min(len(sorted_vals) - 1, int(p * (len(sorted_vals) - 1))))
    return sorted_vals[idx]

def fmt_us(x):
    """Format µs value: sub-10 µs with 2 decimals (sub-µs included), else int."""
    if x < 10:
        return f"{x:.2f}"
    return f"{int(round(x))}"

# Load
data = {}
data_local = {}
data_xhost = {}
for op, _, _ in OPS:
    data[op]       = load(os.path.join(RDIR, f"{op}_lat-Ap.txt"))
    data_local[op] = load(os.path.join(RDIR, f"{op}_local_lat-Ap.txt"))
    data_xhost[op] = load(os.path.join(RDIR, f"{op}_xhost_lat-Ap.txt"))

# CSV summary
csv_path = os.path.join(RDIR, "microbench_summary.csv")
with open(csv_path, "w") as f:
    w = csv.writer(f)
    w.writerow(["op", "direction", "count", "avg_us", "p50_us", "p90_us",
                "p99_us", "p999_us", "min_us", "max_us"])
    for op, _, _ in OPS:
        for dname, v in [("all", data[op]),
                          ("local", data_local[op]),
                          ("xhost", data_xhost[op])]:
            if not v:
                continue
            w.writerow([op, dname, len(v),
                        f"{sum(v)/len(v):.3f}",
                        f"{pct(v, 0.50):.3f}", f"{pct(v, 0.90):.3f}",
                        f"{pct(v, 0.99):.3f}", f"{pct(v, 0.999):.3f}",
                        f"{v[0]:.3f}", f"{v[-1]:.3f}"])
print(f"wrote {csv_path}")

# Plot 1: overall, F-style.
fig, axes = plt.subplots(2, 2, figsize=(12, 8))
for ax, (op, title, color) in zip(axes.flat, OPS):
    v = data[op]
    if not v:
        ax.set_title(title + " (no data)")
        continue
    n = len(v)
    ys = [(i + 1) / n for i in range(n)]
    ax.plot(v, ys, linewidth=2, color=color,
            label="Protocol A (per-host bucket partition)")
    p50, p99 = pct(v, 0.50), pct(v, 0.99)
    ax.axvline(p50, color=color, alpha=0.3, linestyle=":")
    ax.axvline(p99, color=color, alpha=0.5, linestyle="--")
    ax.annotate(f"p50={fmt_us(p50)}µs", xy=(p50, 0.50),
                xytext=(max(p50, 1) * 1.6, 0.45), color=color, fontsize=9,
                arrowprops=dict(arrowstyle="-", color=color, alpha=0.3))
    ax.annotate(f"p99={fmt_us(p99)}µs", xy=(p99, 0.99),
                xytext=(max(p99, 1) * 1.4, 0.92), color=color, fontsize=9,
                arrowprops=dict(arrowstyle="-", color=color, alpha=0.5))
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.set_ylim(0, 1.02)
    xmax = max(v[int(0.999 * (n - 1))] * 1.2, 5)
    ax.set_xlim(0, xmax)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9, loc="lower right")
fig.suptitle("Protocol A microbenchmark — single-client latency CDFs "
             "(FUSEE Fig. 10 counterpart)", fontsize=12, y=1.01)
fig.tight_layout()
out = os.path.join(RDIR, "microbench_cdf.png")
fig.savefig(out, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {out}")

# Plot 2: local vs xhost split (only ops with both files).
fig, axes = plt.subplots(2, 2, figsize=(12, 8))
for ax, (op, title, color) in zip(axes.flat, OPS):
    vl = data_local[op]
    vx = data_xhost[op]
    if not vl and not vx:
        ax.set_title(title + " (no data)")
        continue
    plotted_any = False
    for direction, v, dcolor, dstyle in [
        ("local", vl, "tab:green", "-"),
        ("xhost", vx, "tab:red",   "-"),
    ]:
        if not v: continue
        n = len(v)
        ys = [(i + 1) / n for i in range(n)]
        ax.plot(v, ys, linewidth=2, color=dcolor, linestyle=dstyle,
                label=f"{direction} (n={n})")
        p50, p99 = pct(v, 0.50), pct(v, 0.99)
        ax.axvline(p50, color=dcolor, alpha=0.3, linestyle=":")
        ax.axvline(p99, color=dcolor, alpha=0.5, linestyle="--")
        # stagger annotations vertically so local/xhost labels don't collide
        y_p50 = 0.45 if direction == "local" else 0.30
        y_p99 = 0.92 if direction == "local" else 0.78
        ax.annotate(f"{direction} p50={fmt_us(p50)}µs", xy=(p50, 0.50),
                    xytext=(max(p50, 1) * 1.2 + 0.5, y_p50),
                    color=dcolor, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=dcolor, alpha=0.3))
        ax.annotate(f"{direction} p99={fmt_us(p99)}µs", xy=(p99, 0.99),
                    xytext=(max(p99, 1) * 1.05 + 0.5, y_p99),
                    color=dcolor, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=dcolor, alpha=0.5))
        plotted_any = True
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.set_ylim(0, 1.02)
    cand_xmax = []
    for v in (vl, vx):
        if v:
            cand_xmax.append(v[int(0.999 * (len(v) - 1))] * 1.2)
    xmax = max(cand_xmax + [5]) if cand_xmax else 20
    ax.set_xlim(0, xmax)
    ax.grid(True, alpha=0.3)
    if plotted_any:
        ax.legend(fontsize=9, loc="lower right")
fig.suptitle("Protocol A microbenchmark — local vs cross-host latency CDFs",
             fontsize=12, y=1.01)
fig.tight_layout()
out2 = os.path.join(RDIR, "microbench_cdf_split.png")
fig.savefig(out2, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {out2}")

# Markdown percentile table (re-emit with new file).
md = os.path.join(RDIR, "fig10_summary.md")
with open(md, "w") as fp:
    fp.write("# Protocol A Fig 10 — single-client latency percentiles (µs)\n\n")
    fp.write("| op | direction | n | p50 | p90 | p99 | p99.9 | p99.99 |\n")
    fp.write("|---|---|---|---|---|---|---|---|\n")
    for op, _, _ in OPS:
        for dname, v in [("all", data[op]),
                          ("local", data_local[op]),
                          ("xhost", data_xhost[op])]:
            if not v:
                fp.write(f"| {op.upper()} | {dname} | 0 | n/a | n/a | n/a | n/a | n/a |\n")
                continue
            fp.write(f"| {op.upper()} | {dname} | {len(v)} | "
                     f"{fmt_us(pct(v, 0.50))} | {fmt_us(pct(v, 0.90))} | "
                     f"{fmt_us(pct(v, 0.99))} | {fmt_us(pct(v, 0.999))} | "
                     f"{fmt_us(pct(v, 0.9999))} |\n")
print(f"wrote {md}")
