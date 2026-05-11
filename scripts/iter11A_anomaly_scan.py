#!/usr/bin/env python3
"""iter-11A Phase 6.B — anomaly scan + bimodal gate-12.

Parses a scaling_ycsb sweep SUMMARY.log line-per-cell and applies the
two anomaly tests from scaling_ycsb_spec.md §13 gate 5:

  (a) trans_agg_thpt < 0.1 Mops/s  (absolute floor)
  (b) trans_agg_thpt < geomean(neighbors) / 10
      where neighbors = same (workload, kv, cache), adjacent T values

Outputs:
  - <outdir>/anomaly_cells.txt   one "wl T cache kv" per line
  - <outdir>/gap_to_target.md    per-workload table + anomaly section

Usage:
  scripts/iter11A_anomaly_scan.py --summary SUMMARY.log --out outdir
"""

import argparse
import math
import re
import sys
from collections import defaultdict
from pathlib import Path


LINE_RE = re.compile(
    r"^YCSB opt=A cache=(\d+) num_hosts=\d+ threads=(\d+) threads_eff=\d+ "
    r"rep=\d+ load_ops=\d+ load_thpt=\d+ trans_ops=\d+ trans_wall_max=[\d.]+ "
    r"trans_agg_thpt=(\d+) .*# (\w+)_optA_t\d+_cache(on|off)_rep\d+_kv(\d+)"
)


def parse_summary(path):
    cells = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            m = LINE_RE.match(line)
            if not m:
                continue
            cache_int, T, thpt, wl, cache_word, kv = m.groups()
            cells.append({
                "workload": wl,
                "T": int(T),
                "cache": cache_word,
                "kv": int(kv),
                "thpt": int(thpt),
                "mops": int(thpt) / 1_000_000.0,
            })
    return cells


def geomean(xs):
    xs = [x for x in xs if x > 0]
    if not xs:
        return 0.0
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def scan_anomalies(cells):
    # Group by (workload, kv, cache)
    by_grp = defaultdict(list)  # key=(wl,kv,cache) -> [(T, mops)]
    for c in cells:
        by_grp[(c["workload"], c["kv"], c["cache"])].append((c["T"], c["mops"]))

    anomalies = []
    for key, ts in by_grp.items():
        ts.sort()
        mops_by_t = {t: m for t, m in ts}
        ts_sorted = sorted(mops_by_t.keys())
        for i, t in enumerate(ts_sorted):
            m = mops_by_t[t]
            neighbors = []
            if i > 0:
                neighbors.append(mops_by_t[ts_sorted[i - 1]])
            if i < len(ts_sorted) - 1:
                neighbors.append(mops_by_t[ts_sorted[i + 1]])
            ngeo = geomean(neighbors) if neighbors else 0.0
            reasons = []
            if m < 0.1:
                reasons.append(f"abs<0.1 ({m:.4f})")
            if neighbors and ngeo > 0 and m < ngeo / 10:
                reasons.append(f"nbr/10 ({m:.4f} < {ngeo/10:.4f})")
            if reasons:
                wl, kv, cache = key
                anomalies.append({
                    "workload": wl, "T": t, "cache": cache, "kv": kv,
                    "mops": m, "reasons": reasons,
                })
    return anomalies


def render_gap_to_target(cells, anomalies, out_path):
    BAR = 20.0
    lines = []
    lines.append("# iter-11A Phase 6 — gap_to_target.md")
    lines.append("")
    lines.append("Sweep headlines per workload (best cell) and the gap to "
                 "the 20 Mops/s success bar.")
    lines.append("")
    by_wl = defaultdict(list)
    for c in cells:
        by_wl[c["workload"]].append(c)
    lines.append("| workload | best Mops/s | gap to 20 (Mops/s) | best cell |")
    lines.append("|---|---:|---:|---|")
    for wl in sorted(by_wl.keys()):
        wl_cells = by_wl[wl]
        best = max(wl_cells, key=lambda c: c["mops"])
        gap = BAR - best["mops"]
        cell = f"T={best['T']} cache={best['cache']} kv={best['kv']}"
        lines.append(f"| {wl} | {best['mops']:.3f} | {gap:.3f} | {cell} |")
    lines.append("")
    lines.append("## §13 gate 5 anomaly scan")
    lines.append("")
    lines.append(f"Total cells: {len(cells)}; anomalous cells flagged: "
                 f"{len(anomalies)}.")
    lines.append("")
    lines.append("| workload | T | cache | kv | thpt Mops/s | reason |")
    lines.append("|---|---:|---|---:|---:|---|")
    for a in anomalies:
        lines.append(
            f"| {a['workload']} | {a['T']} | {a['cache']} | {a['kv']} "
            f"| {a['mops']:.4f} | {'; '.join(a['reasons'])} |"
        )
    out_path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    cells = parse_summary(args.summary)
    if not cells:
        print(f"ERROR: parsed 0 cells from {args.summary}", file=sys.stderr)
        sys.exit(1)
    print(f"parsed {len(cells)} cells")

    anomalies = scan_anomalies(cells)
    print(f"flagged {len(anomalies)} anomalies")

    # Write anomaly_cells.txt
    ac = out / "anomaly_cells.txt"
    with ac.open("w") as f:
        for a in anomalies:
            f.write(f"{a['workload']} {a['T']} {a['cache']} {a['kv']}\n")
    print(f"wrote {ac}")

    gap = out / "gap_to_target.md"
    render_gap_to_target(cells, anomalies, gap)
    print(f"wrote {gap}")


if __name__ == "__main__":
    main()
