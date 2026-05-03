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
    r"# (\w+)_optA_t(\d+)_cache(on|off)_rep(\d+)(?:_kv(\d+))?$"
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
             wl, T2, cache_str, rep2, kv) = m.groups()
            cell = (wl, int(T), cache_str, int(kv) if kv else 0)
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
        wl, T, cache, kv = cell
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
            "kv": kv,
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
        f.write("# Protocol A scaling_ycsb summary (median)\n\n")
        f.write("| Workload | T | Cache | KV | Reps | Mops/s (med) | Range Mops | w_p99 µs | r_p99 µs | Unstable? |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['workload']} | {r['T']} | {r['cache']} | {r['kv']} | {r['n_reps']} "
                    f"| {r['thpt_med_mops']:.3f} "
                    f"| {r['thpt_min_ops']/1e6:.3f}-{r['thpt_max_ops']/1e6:.3f} "
                    f"| {r['w_p99_ns_med']/1000:.1f} | {r['r_p99_ns_med']/1000:.1f} "
                    f"| {'YES' if r['unstable'] else 'no'} |\n")


def anomaly_scan(rows):
    """iter-7A §13 gate 5: dual-condition threshold for outliers.

    Cell flagged if Mops/s < 0.1 absolute OR Mops/s < (geomean of
    same-(wl, kv) T-neighbors) / 10. Returns list of anomalies.
    """
    import math
    by_group = {}
    for r in rows:
        key = (r["workload"], r["kv"], r["cache"])
        by_group.setdefault(key, []).append(r)
    anomalies = []
    for key, runs in by_group.items():
        runs.sort(key=lambda r: r["T"])
        for i, r in enumerate(runs):
            mops = r["thpt_med_mops"]
            if mops <= 0:
                continue  # FAILs handled separately
            # Find same-(wl, kv) T-neighbors (other Ts).
            neighbors = [x["thpt_med_mops"] for j, x in enumerate(runs) if j != i and x["thpt_med_mops"] > 0]
            geomean = 0
            if len(neighbors) >= 2:
                logs = [math.log(v) for v in neighbors]
                geomean = math.exp(sum(logs) / len(logs))
            cond1 = mops < 0.1
            cond2 = geomean > 0 and mops < geomean / 10
            if cond1 or cond2:
                reason = []
                if cond1: reason.append("< 0.1 abs")
                if cond2: reason.append(f"< neighbor-geomean({geomean:.3f})/10")
                anomalies.append({
                    "wl": r["workload"], "kv": r["kv"], "T": r["T"],
                    "cache": r["cache"], "mops": mops, "reason": "; ".join(reason),
                })
    return anomalies


def write_gap_to_target(rows, path):
    """Distance to 20 Mops/s per (workload, KV, T) cell. Cache=on only for headlines."""
    with open(path, "w") as f:
        f.write("# Gap to 20 Mops/s target (per `docs/design_goals.md`)\n\n")
        f.write("Peak Mops/s per (workload, KV size), cache=on.\n\n")
        f.write("| Workload | KV | Peak Mops/s | At T= | Gap to 20 Mops/s |\n")
        f.write("|---|---|---|---|---|\n")
        peaks = {}
        for r in rows:
            if r["cache"] != "on":
                continue
            key = (r["workload"], r["kv"])
            if key not in peaks or r["thpt_med_mops"] > peaks[key][0]:
                peaks[key] = (r["thpt_med_mops"], r["T"])
        for k in sorted(peaks):
            wl, kv = k
            mops, T = peaks[k]
            gap = TARGET_MOPS - mops
            f.write(f"| {wl} | {kv} | {mops:.3f} | {T} | {gap:+.2f} ({(mops / TARGET_MOPS * 100):.1f}% of target) |\n")
        f.write("\n## All cells (cache=on)\n\n")
        f.write("| Workload | KV | T | Mops/s | %% of 20 Mops/s |\n")
        f.write("|---|---|---|---|---|\n")
        for r in rows:
            if r["cache"] != "on":
                continue
            f.write(f"| {r['workload']} | {r['kv']} | {r['T']} | {r['thpt_med_mops']:.4f} "
                    f"| {(r['thpt_med_mops'] / TARGET_MOPS * 100):.2f}% |\n")


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
    # iter-7A §13 gate 5 anomaly scan, appended to gap_to_target.md.
    anomalies = anomaly_scan(rows)
    with open(outdir / "gap_to_target.md", "a") as f:
        f.write("\n\n## §13 gate 5 anomaly scan (dual-condition threshold)\n\n")
        f.write("Threshold: cell flagged if Mops/s < 0.1 absolute OR < (same-(wl, kv) T-neighbor geomean) / 10.\n\n")
        if not anomalies:
            f.write("**ZERO unexplained anomalies.** §13 gate 5: PASS.\n")
        else:
            f.write(f"**{len(anomalies)} anomaly cells** flagged (HARD FAIL on §13 gate 5 unless explained):\n\n")
            f.write("| Workload | KV | T | Cache | Mops/s | Reason |\n")
            f.write("|---|---|---|---|---|---|\n")
            for a in anomalies:
                f.write(f"| {a['wl']} | {a['kv']} | {a['T']} | {a['cache']} | "
                        f"{a['mops']:.4f} | {a['reason']} |\n")
            f.write("\n**Per spec §13 gate 5 (added iter-7A 2026-05-03)**: each anomaly "
                    "must be explained with **5-rep multi-rep evidence** of \"genuine "
                    "noise, not a bug\", OR the iter cannot be marked COMPLETE. "
                    "\"Single-rep noise\" tag without 5-rep evidence is the iter-6A "
                    "failure pattern explicitly forbidden.\n")
    print(f"wrote summary_table.md ({len(rows)} cells, {len(unstable)} unstable)")
    print(f"wrote gap_to_target.md")
    if anomalies:
        print(f"§13 gate 5: {len(anomalies)} anomalies flagged")
        sys.exit(1)
    else:
        print(f"§13 gate 5: PASS (zero anomalies)")
    if unstable:
        print(f"unstable cells (> 20% spread): {unstable}")


if __name__ == "__main__":
    main()
