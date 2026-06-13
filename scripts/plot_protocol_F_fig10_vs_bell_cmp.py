#!/usr/bin/env python3
"""Compare FUSEE-CXL (LFM, g1/g2) Fig 10 latency CDFs against the FUSEE-RDMA
baseline measured on bell_g1 (docs/bell_g1_benchmark_20260607/fig1_latency_cdf.png).

Overlays the bell rep1 µs distribution on each of the 4 panels in
docs/protocol_F_fig10_1024B_20260608_062956/microbench_cdf.png. FUSEE-CXL
uses the saturated color; FUSEE-RDMA uses the same-family lighter shade.

Outputs:
  docs/protocol_F_fig10_1024B_20260608_062956/microbench_cdf_cmp.png
"""
import os, csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

F_DIR    = "docs/protocol_F_fig10_1024B_20260608_062956"
BELL_DIR = "docs/bell_g1_benchmark_20260607/raw"
OUT      = os.path.join(F_DIR, "microbench_cdf_cmp.png")

# (op, panel title, F-color (saturated), Bell-color (faded same-family))
OPS = [
    ("insert", "(a) INSERT latency CDF", "tab:red",    "lightcoral"),
    ("update", "(b) UPDATE latency CDF", "tab:orange", "navajowhite"),
    ("search", "(c) SEARCH latency CDF", "tab:blue",   "lightsteelblue"),
    ("delete", "(d) DELETE latency CDF", "tab:green",  "lightgreen"),
]

# bell rep1 uses lat_<op>_rep1.txt, except delete uses the FIXED file (per
# Bell's plot script: lat_delete_FIXED_rep1.txt, since the unfixed file
# excludes the deletion-recovery hangs).
BELL_FILE = {
    "insert": "lat_insert_rep1.txt",
    "update": "lat_update_rep1.txt",
    "search": "lat_search_rep1.txt",
    "delete": "lat_delete_FIXED_rep1.txt",
}

def load(path):
    if not os.path.exists(path):
        return []
    vals = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: vals.append(int(line))
            except ValueError: pass
    return sorted(vals)

def pct(v, p):
    if not v: return 0
    i = max(0, min(len(v) - 1, int(p * (len(v) - 1))))
    return v[i]

fig, axes = plt.subplots(2, 2, figsize=(12, 8))
for ax, (op, title, f_color, b_color) in zip(axes.flat, OPS):
    f_path = os.path.join(F_DIR, f"{op}_lat-Fp.txt")
    b_path = os.path.join(BELL_DIR, BELL_FILE[op])
    vf = load(f_path)
    vb = load(b_path)

    plotted = False
    for label, v, color, alpha, lstyle in [
        (f"FUSEE-CXL (LFM)", vf, f_color, 1.0, "-"),
        (f"FUSEE-RDMA",    vb, b_color, 0.95, "-"),
    ]:
        if not v: continue
        n = len(v)
        ys = [(i + 1) / n for i in range(n)]
        ax.plot(v, ys, linewidth=2, color=color, alpha=alpha, linestyle=lstyle,
                label=label)
        p50, p99 = pct(v, 0.50), pct(v, 0.99)
        ax.axvline(p50, color=color, alpha=alpha * 0.35, linestyle=":")
        ax.axvline(p99, color=color, alpha=alpha * 0.55, linestyle="--")
        plotted = True

    # Range: max of both p99 × 1.6, floor 5 µs. Avoids letting bell's
    # >3 ms recovery-hang tail (delete only) stretch the panel and bury F.
    cand = []
    for v in (vf, vb):
        if v:
            cand.append(pct(v, 0.99) * 1.6)
    xmax = max(cand + [5]) if cand else 25
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)

    # Annotate p50/p99 for both, staggered.
    if vf:
        p50f, p99f = pct(vf, 0.50), pct(vf, 0.99)
        ax.annotate(f"CXL p50={p50f}µs", xy=(p50f, 0.50),
                    xytext=(max(p50f, 1) * 1.3 + 0.5, 0.45),
                    color=f_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=f_color, alpha=0.3))
        ax.annotate(f"CXL p99={p99f}µs", xy=(p99f, 0.99),
                    xytext=(max(p99f, 1) * 1.05 + 0.5, 0.92),
                    color=f_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=f_color, alpha=0.5))
    if vb:
        p50b, p99b = pct(vb, 0.50), pct(vb, 0.99)
        ax.annotate(f"RDMA p50={p50b}µs", xy=(p50b, 0.50),
                    xytext=(max(p50b, 1) * 1.3 + 0.5, 0.30),
                    color=b_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=b_color, alpha=0.5))
        ax.annotate(f"RDMA p99={p99b}µs", xy=(p99b, 0.99),
                    xytext=(max(p99b, 1) * 1.05 + 0.5, 0.77),
                    color=b_color, fontsize=8,
                    arrowprops=dict(arrowstyle="-", color=b_color, alpha=0.6))

    if plotted:
        ax.legend(fontsize=9, loc="lower right")

fig.suptitle("Single-client latency CDFs — FUSEE-CXL (LFM) vs FUSEE-RDMA",
             fontsize=12, y=1.01)
fig.tight_layout()
fig.savefig(OUT, dpi=140, bbox_inches="tight")
plt.close(fig)
print(f"wrote {OUT}")

# Also dump a comparison table.
csv_out = os.path.join(F_DIR, "microbench_summary_cmp.csv")
with open(csv_out, "w") as f:
    w = csv.writer(f)
    w.writerow(["op", "source", "n", "p50_us", "p90_us", "p99_us", "p999_us",
                "min_us", "max_us"])
    for op, _, _, _ in OPS:
        vf = load(os.path.join(F_DIR, f"{op}_lat-Fp.txt"))
        vb = load(os.path.join(BELL_DIR, BELL_FILE[op]))
        for src, v in [("protocol_F", vf), ("bell_g1", vb)]:
            if not v:
                continue
            w.writerow([op, src, len(v),
                        pct(v, 0.50), pct(v, 0.90), pct(v, 0.99),
                        pct(v, 0.999), v[0], v[-1]])
print(f"wrote {csv_out}")
