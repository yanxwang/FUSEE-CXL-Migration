#!/usr/bin/env python3
"""iter-5 (kv x N) peak-throughput grid plots.

Reads multiple SUMMARY.log files indexed by (vsize, N) and emits:
  - per-workload heatmap PNG (rows = vsize, cols = N, cell = peak Mops/s)
  - per-workload N-comparison line plot (x=T, lines per N) at one vsize
  - cross-vsize, cross-N peak summary bar chart

Usage:
  python3 plot_iter5_kv_n_grid.py <out_dir> kv<vs>_N<n>=<sum_path> ...

Example:
  python3 plot_iter5_kv_n_grid.py docs/iter5_kv_n_compare \\
    kv256_N1=.../SUMMARY.log kv256_N2=.../SUMMARY.log kv256_N4=...\\
    kv512_N1=... ... kv1024_N1=... ...
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
    r"num_hosts=\d+\s+threads=(?P<T>\d+)(?:\s+threads_eff=\d+)?\s+"
    r"load_ops=\d+\s+load_thpt=[\d.]+\s+"
    r"trans_ops=\d+\s+trans_wall_max=[\d.]+s\s+"
    r"trans_agg_thpt=(?P<thpt>[\d.]+)\s+"
    r"w_avg_ns=\d+\s+w_p50_ns=\d+\s+w_p99_ns=(?P<wp99>\d+)\s+"
)
TAG = re.compile(r"#\s*(?P<wl>workload\w)_opt[ABC]_t\d+_cache(?P<c>on|off)")
WORKLOADS = ["workloada","workloadb","workloadc","workloadd","workloadf"]
TS = [1, 2, 4, 8, 16, 32, 64, 86]

def parse(path, want_cache=1):
    out = {}  # (wl, T) -> (thpt_mops, wp99_us)
    if not os.path.exists(path): return out
    with open(path) as fh:
        for line in fh:
            m = YCSB.search(line)
            if not m or m.group("opt") != "C" or int(m.group("c")) != want_cache:
                continue
            t = TAG.search(line)
            if not t: continue
            out[(t.group("wl"), int(m.group("T")))] = (
                float(m.group("thpt"))/1e6, int(m.group("wp99"))/1000.0)
    return out

def peak(parsed, wl):
    best = 0.0
    for T in TS:
        v = parsed.get((wl, T), (0.0, 0))[0]
        if v > best: best = v
    return best

def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr); sys.exit(2)
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    cells = {}  # (vs, N) -> parsed dict
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print(f"bad arg {arg!r}", file=sys.stderr); sys.exit(2)
        label, path = arg.split("=", 1)
        m = re.match(r"kv(\d+)_N(\d+)$", label)
        if not m:
            print(f"bad label {label!r}, expected kv<vs>_N<n>", file=sys.stderr); sys.exit(2)
        cells[(int(m.group(1)), int(m.group(2)))] = parse(path)

    vs_list = sorted({k[0] for k in cells})
    N_list = sorted({k[1] for k in cells})

    # Per-workload heatmap.
    for wl in WORKLOADS:
        grid = np.zeros((len(vs_list), len(N_list)))
        for i, vs in enumerate(vs_list):
            for j, N in enumerate(N_list):
                grid[i, j] = peak(cells.get((vs, N), {}), wl)
        fig, ax = plt.subplots(figsize=(5.5, 3.8))
        im = ax.imshow(grid, cmap="viridis", aspect="auto")
        ax.set_xticks(range(len(N_list)));  ax.set_xticklabels([f"N={n}" for n in N_list])
        ax.set_yticks(range(len(vs_list))); ax.set_yticklabels([f"kv{vs}" for vs in vs_list])
        ax.set_title(f"C — {wl} — peak thpt (Mops/s, cache=on)")
        for i in range(len(vs_list)):
            for j in range(len(N_list)):
                ax.text(j, i, f"{grid[i, j]:.1f}", ha="center", va="center",
                        color="white" if grid[i, j] < grid.max()*0.6 else "black", fontsize=9)
        ax.axhline(y=-0.5)  # placeholder for visual; the real 20 Mops/s bar shown via a line annotation:
        fig.colorbar(im, ax=ax)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_kv_n_grid_{wl}.png"), dpi=110)
        plt.close(fig)

    # N-comparison line plot at each vsize: x=T, lines per N.
    for vs in vs_list:
        for wl in WORKLOADS:
            fig, ax = plt.subplots(figsize=(7, 4))
            for N in N_list:
                p = cells.get((vs, N), {})
                xs, ys = [], []
                for T in TS:
                    if (wl, T) in p:
                        xs.append(T); ys.append(p[(wl, T)][0])
                if xs: ax.plot(xs, ys, marker="o", label=f"N={N}")
            ax.axhline(20, ls="--", color="gray", label="20 Mops/s bar")
            ax.set_xscale("log", base=2)
            ax.set_xticks(TS); ax.set_xticklabels([str(x) for x in TS])
            ax.set_xlabel("#clients per host (T)"); ax.set_ylabel("trans_agg_thpt (Mops/s)")
            ax.set_title(f"C — {wl} — kv{vs} — flusher count compare (cache=on)")
            ax.legend(fontsize=8); ax.grid(alpha=0.3); fig.tight_layout()
            fig.savefig(os.path.join(out_dir, f"C_kv{vs}_N_compare_{wl}.png"), dpi=100)
            plt.close(fig)

    # Per-workload bar of peak across all (vs, N) cells.
    for wl in WORKLOADS:
        fig, ax = plt.subplots(figsize=(8, 4.5))
        labels, peaks = [], []
        for vs in vs_list:
            for N in N_list:
                labels.append(f"kv{vs}/N{N}")
                peaks.append(peak(cells.get((vs, N), {}), wl))
        x = list(range(len(labels)))
        ax.bar(x, peaks)
        ax.axhline(20, ls="--", color="gray", label="20 Mops/s bar")
        ax.set_xticks(x); ax.set_xticklabels(labels, rotation=30, fontsize=8)
        ax.set_ylabel("peak trans_agg_thpt (Mops/s, cache=on)")
        ax.set_title(f"C — {wl} — peak across (vsize, N) cells")
        ax.legend(); ax.grid(axis="y", alpha=0.3); fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"C_iter5_peak_bar_{wl}.png"), dpi=100)
        plt.close(fig)

if __name__ == "__main__":
    main()
