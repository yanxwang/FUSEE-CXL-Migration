#!/usr/bin/env python3
"""iter-4A-redo summarizer + gap-to-target generator.

Reads SUMMARY.log produced by tests/protocol_a_ycsb (per
docs/scaling_ycsb_spec.md §8) and emits:

  - summary_table.md:    per-cell 5-rep median throughput + write/read p99 latencies
  - gap_to_target.md:    distance-to-20-Mops/s for each (workload, T) cell
  - A_thpt_workload<X>.csv: throughput data for plot scripts
"""
import os
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# YCSB opt=A cache=1 num_hosts=2 threads=2 threads_eff=2 rep=1 ...
LINE = re.compile(
    r"YCSB opt=A cache=(\d+) num_hosts=(\d+) threads=(\d+) threads_eff=\d+ "
    r"rep=(\d+) load_ops=(\d+) load_thpt=(\d+) "
    r"trans_ops=(\d+) trans_wall_max=([\d\.]+) trans_agg_thpt=(\d+) "
    r"w_avg_ns=(\d+) w_p50_ns=(\d+) w_p99_ns=(\d+) "
    r"r_avg_ns=(\d+) r_p50_ns=(\d+) r_p99_ns=(\d+) "
    r"# (\w+)_optA_t(\d+)_cache(on|off)_rep(\d+)$"
)

TARGET_MOPS = 20.0


def parse_summary(path):
    by_cell = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("FAIL"):
                continue
            m = LINE.match(line)
            if not m:
                continue
            (cache, _hosts, T, rep, load_ops, load_thpt,
             trans_ops, wall, agg_thpt,
             w_avg, w_p50, w_p99, r_avg, r_p50, r_p99,
             wl, T2, cache_str, rep2) = m.groups()
            cell = (wl, int(T), cache_str)
            by_cell[cell].append({
                "rep": int(rep),
                "trans_agg_thpt": int(agg_thpt),
                "trans_wall_s": float(wall),
                "load_thpt": int(load_thpt),
                "w_p50_ns": int(w_p50),
                "w_p99_ns": int(w_p99),
                "r_p50_ns": int(r_p50),
                "r_p99_ns": int(r_p99),
            })
    return by_cell


def median(xs):
    return statistics.median(xs) if xs else 0


def summarize(by_cell, outdir):
    rows = []
    cells_unstable = []
    for cell, runs in sorted(by_cell.items()):
        wl, T, cache = cell
        thpt = sorted(r["trans_agg_thpt"] for r in runs)
        med = median(thpt)
        spread = (max(thpt) - min(thpt)) / med if med else 0
        unstable = spread > 0.20
        if unstable:
            cells_unstable.append(cell)
        rows.append({
            "workload": wl,
            "T": T,
            "cache": cache,
            "n_reps": len(runs),
            "thpt_med_ops": med,
            "thpt_med_mops": med / 1e6,
            "thpt_min_ops": min(thpt),
            "thpt_max_ops": max(thpt),
            "spread": spread,
            "unstable": unstable,
            "w_p99_ns_med": median(sorted(r["w_p99_ns"] for r in runs)),
            "r_p99_ns_med": median(sorted(r["r_p99_ns"] for r in runs)),
        })
    return rows, cells_unstable


def write_summary_table(rows, path):
    with open(path, "w") as f:
        f.write("# Protocol A scaling_ycsb summary (5-rep median)\n\n")
        f.write("| Workload | T | Cache | Reps | Mops/s (med) | Range (min-max kops) | w_p99 µs | r_p99 µs | Unstable? |\n")
        f.write("|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['workload']} | {r['T']} | {r['cache']} | {r['n_reps']} "
                    f"| {r['thpt_med_mops']:.3f} "
                    f"| {r['thpt_min_ops']/1e6:.3f}-{r['thpt_max_ops']/1e6:.3f} Mops "
                    f"| {r['w_p99_ns_med']/1000:.1f} | {r['r_p99_ns_med']/1000:.1f} "
                    f"| {'YES' if r['unstable'] else 'no'} |\n")


def write_gap_to_target(rows, path):
    """Distance to 20 Mops/s per (workload, T) cell. Cache=on only for headlines."""
    with open(path, "w") as f:
        f.write("# Gap to 20 Mops/s target (per `docs/design_goals.md`)\n\n")
        f.write("Headline: peak Mops/s per workload (cache=on, all T values).\n\n")
        f.write("| Workload | Peak Mops/s | At T= | Gap to 20 Mops/s |\n")
        f.write("|---|---|---|---|\n")
        peaks = {}
        for r in rows:
            if r["cache"] != "on":
                continue
            wl = r["workload"]
            if wl not in peaks or r["thpt_med_mops"] > peaks[wl][0]:
                peaks[wl] = (r["thpt_med_mops"], r["T"])
        for wl in sorted(peaks):
            mops, T = peaks[wl]
            gap = TARGET_MOPS - mops
            f.write(f"| {wl} | {mops:.2f} | {T} | {gap:+.2f} ({(mops / TARGET_MOPS * 100):.1f}% of target) |\n")
        f.write("\n## All cells (cache=on)\n\n")
        f.write("| Workload | T | Mops/s | %% of 20 Mops/s |\n")
        f.write("|---|---|---|---|\n")
        for r in rows:
            if r["cache"] != "on":
                continue
            f.write(f"| {r['workload']} | {r['T']} | {r['thpt_med_mops']:.3f} "
                    f"| {(r['thpt_med_mops'] / TARGET_MOPS * 100):.1f}% |\n")


def main():
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    summary_path = outdir / "SUMMARY.log"
    by_cell = parse_summary(summary_path)
    if not by_cell:
        print(f"no parsable lines in {summary_path}", file=sys.stderr)
        sys.exit(1)
    rows, unstable = summarize(by_cell, outdir)
    write_summary_table(rows, outdir / "summary_table.md")
    write_gap_to_target(rows, outdir / "gap_to_target.md")
    print(f"wrote summary_table.md ({len(rows)} cells, {len(unstable)} unstable)")
    print(f"wrote gap_to_target.md")
    if unstable:
        print(f"unstable cells (> 20% spread): {unstable}")


if __name__ == "__main__":
    main()
