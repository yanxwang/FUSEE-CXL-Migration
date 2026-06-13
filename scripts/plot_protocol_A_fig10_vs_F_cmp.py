#!/usr/bin/env python3
"""Compare FUSEE-CXL Protocol A (per-host bucket partition) Fig 10 latency CDFs
against the FUSEE-CXL (LFM) baseline measured under the same
g1+g2 testbed (docs/protocol_F_fig10_1024B_20260608_062956/microbench_cdf.png).

Overlays Protocol F's rep1 µs distribution on each of the 4 panels in
docs/protocol_A_fig10_20260609_192025/results/microbench_cdf.png. Protocol A
uses the saturated color (matches single-protocol plot); Protocol F uses the
same-family lighter shade.

Outputs:
  docs/protocol_A_fig10_20260609_192025/results/microbench_cdf_cmp.png
  docs/protocol_A_fig10_20260609_192025/results/microbench_summary_cmp.csv
"""
import os, csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

A_DIR = "docs/protocol_A_fig10_20260609_192025/results"
F_DIR = "docs/protocol_F_fig10_1024B_20260608_062956"
OUT   = os.path.join(A_DIR, "microbench_cdf_cmp.png")

# (op, panel title, A-color (saturated), F-color (faded same-family))
OPS = [
    ("insert", "(a) INSERT latency CDF", "tab:red",    "lightcoral"),
    ("update", "(b) UPDATE latency CDF", "tab:orange", "navajowhite"),
    ("search", "(c) SEARCH latency CDF", "tab:blue",   "lightsteelblue"),
    ("delete", "(d) DELETE latency CDF", "tab:green",  "lightgreen"),
]

def load(path):
    """Accepts int OR float entries (A files are float-µs after iter-21A zero
    replacement; F files are int-µs). Returns sorted µs floats."""
    if not os.path.exists(path):
        return []
    vals = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: vals.append(float(line))
            except ValueError: pass
    return sorted(vals)

def pct(v, p):
    if not v: return 0.0
    i = max(0, min(len(v) - 1, int(p * (len(v) - 1))))
    return v[i]

def fmt_us(x):
    if x < 10:
        return f"{x:.2f}"
    return f"{int(round(x))}"

fig, axes = plt.subplots(2, 2, figsize=(12, 8))
for ax, (op, title, a_color, f_color) in zip(axes.flat, OPS):
    a_path = os.path.join(A_DIR, f"{op}_lat-Ap.txt")
    f_path = os.path.join(F_DIR, f"{op}_lat-Fp.txt")
    va = load(a_path)
    vf = load(f_path)

    plotted = False
    for label, v, color, alpha in [
        ("FUSEE-CXL (Host partition)", va, a_color, 1.0),
        ("FUSEE-CXL (LFM)",                vf, f_color, 0.95),
    ]:
        if not v: continue
        n = len(v)
        ys = [(i + 1) / n for i in range(n)]
        ax.plot(v, ys, linewidth=2, color=color, alpha=alpha, label=label)
        p50, p99 = pct(v, 0.50), pct(v, 0.99)
        ax.axvline(p50, color=color, alpha=alpha * 0.35, linestyle=":")
        ax.axvline(p99, color=color, alpha=alpha * 0.55, linestyle="--")
        plotted = True

    # Range: max of both p99 × 1.6, floor 5 µs.
    cand = []
    for v in (va, vf):
        if v:
            cand.append(pct(v, 0.99) * 1.6)
    xmax = max(cand + [5]) if cand else 25
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)

    # Annotate p50/p99 for both, vertically staggered.
    if va:
        p50a, p99a = pct(va, 0.50), pct(va, 0.99)
        ax.annotate(f"Host p50={fmt_us(p50a)}µs", xy=(p50a, 0.50),
                    xytext=(max(p50a, 1) * 1.3 + 0.5, 0.45),
                    color=a_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=a_color, alpha=0.3))
        ax.annotate(f"Host p99={fmt_us(p99a)}µs", xy=(p99a, 0.99),
                    xytext=(max(p99a, 1) * 1.05 + 0.5, 0.92),
                    color=a_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=a_color, alpha=0.5))
    if vf:
        p50f, p99f = pct(vf, 0.50), pct(vf, 0.99)
        ax.annotate(f"LFM p50={fmt_us(p50f)}µs", xy=(p50f, 0.50),
                    xytext=(max(p50f, 1) * 1.3 + 0.5, 0.30),
                    color=f_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=f_color, alpha=0.5))
        ax.annotate(f"LFM p99={fmt_us(p99f)}µs", xy=(p99f, 0.99),
                    xytext=(max(p99f, 1) * 1.05 + 0.5, 0.77),
                    color=f_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=f_color, alpha=0.6))

    if plotted:
        ax.legend(fontsize=9, loc="lower right")

fig.suptitle("Single-client latency CDFs — FUSEE-CXL (Host partition) vs FUSEE-CXL (LFM)",
             fontsize=12, y=1.01)
fig.tight_layout()
fig.savefig(OUT, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {OUT}")

# Companion CSV summary.
csv_out = os.path.join(A_DIR, "microbench_summary_cmp.csv")
with open(csv_out, "w") as f:
    w = csv.writer(f)
    w.writerow(["op", "source", "n", "p50_us", "p90_us", "p99_us", "p999_us",
                "min_us", "max_us"])
    for op, _, _, _ in OPS:
        va = load(os.path.join(A_DIR, f"{op}_lat-Ap.txt"))
        vf = load(os.path.join(F_DIR, f"{op}_lat-Fp.txt"))
        for src, v in [("protocol_A", va), ("protocol_F", vf)]:
            if not v:
                continue
            w.writerow([op, src, len(v),
                        f"{pct(v, 0.50):.2f}", f"{pct(v, 0.90):.2f}",
                        f"{pct(v, 0.99):.2f}", f"{pct(v, 0.999):.2f}",
                        f"{v[0]:.2f}", f"{v[-1]:.2f}"])
print(f"wrote {csv_out}")
