#!/usr/bin/env python3
"""iter-11A Phase 5.B + 5.C — generate per-cell iter10A_vs_iter11A_delta.md
plus a consolidated cross-cell summary identifying which Phase-1/2/3/4 fix
actually moved the µs needle and which didn't.

Inputs:
  --iter10a-dir  path to docs/path_decomp_iter10A_<ts>/
  --iter11a-dir  path to docs/path_decomp_iter11A_<ts>/
  --cells-file   path to phase4_cells.txt (label workload kv T cache expected)

Outputs (written into iter-11a-dir):
  per_cell/<label>/iter10A_vs_iter11A_delta.md
  consolidated_iter10A_to_iter11A_delta.md
"""

import argparse
import os
import re
import sys
from pathlib import Path

STAGE_ROW_RE = re.compile(
    r"^\|\s*([A-Za-z][A-Za-z0-9_]*)\s*\|\s*([0-9—✱].*?)\s*\|"
    r"\s*([-0-9.—✱].*?)\s*\|\s*([-0-9.—✱].*?)\s*\|"
    r"\s*([-0-9.—✱].*?)\s*\|\s*([-0-9.—✱].*?)\s*\|"
    r"\s*([-0-9.—✱].*?)\s*\|"
)


def parse_per_stage_md(path):
    """Parse healthy capture table; return dict[stage] = {p50, p90, p99, max, mean}.
    Stages with `✱ no data` get None entries."""
    if not path.exists():
        return {}
    rows = {}
    in_healthy = False
    with path.open() as f:
        for line in f:
            if line.startswith("## Healthy capture"):
                in_healthy = True
                continue
            if line.startswith("## Anomaly") or line.startswith("## ") and in_healthy:
                in_healthy = False
                continue
            if not in_healthy:
                continue
            m = STAGE_ROW_RE.match(line.strip())
            if not m:
                continue
            stage = m.group(1)
            if stage in ("Stage",):  # header row
                continue
            try:
                vals = [v.strip() for v in m.groups()[1:]]
                # vals = [N, p50, p90, p99, max, mean]
                def parse_num(s):
                    if s in ("—", "✱"):
                        return None
                    try:
                        return float(s)
                    except ValueError:
                        return None
                rows[stage] = {
                    "N":    parse_num(vals[0]),
                    "p50":  parse_num(vals[1]),
                    "p90":  parse_num(vals[2]),
                    "p99":  parse_num(vals[3]),
                    "max":  parse_num(vals[4]),
                    "mean": parse_num(vals[5]),
                }
            except (IndexError, ValueError):
                continue
    return rows


def delta_pct(old, new):
    if old is None or new is None or old == 0:
        return None
    return (new - old) / old * 100.0


def fmt_num(x, prec=3):
    return f"{x:.{prec}f}" if x is not None else "—"


def fmt_delta(d):
    if d is None:
        return "—"
    sign = "+" if d >= 0 else ""
    return f"{sign}{d:.1f}%"


def write_per_cell_delta(out_path, label, wl, kv, T, cache, expected,
                          iter10a_rows, iter11a_rows):
    all_stages = sorted(set(iter10a_rows.keys()) | set(iter11a_rows.keys()))
    lines = []
    lines.append(f"# {label} — iter-10A vs iter-11A path_decomp delta")
    lines.append("")
    lines.append(f"**Cell**: workload={wl} kv={kv} T={T} cache={cache} "
                 f"(iter-10A expected ~{expected} Mops/s)")
    lines.append("")
    lines.append("| Stage | i10A p50 µs | i11A p50 µs | p50 Δ% | "
                 "i10A p99 µs | i11A p99 µs | p99 Δ% | "
                 "i10A mean µs | i11A mean µs | mean Δ% |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for stage in all_stages:
        a = iter10a_rows.get(stage, {})
        b = iter11a_rows.get(stage, {})
        ap50, bp50 = a.get("p50"), b.get("p50")
        ap99, bp99 = a.get("p99"), b.get("p99")
        amean, bmean = a.get("mean"), b.get("mean")
        if all(v is None for v in (ap50, bp50, ap99, bp99, amean, bmean)):
            continue
        lines.append(
            f"| {stage} "
            f"| {fmt_num(ap50)} | {fmt_num(bp50)} | {fmt_delta(delta_pct(ap50, bp50))} "
            f"| {fmt_num(ap99)} | {fmt_num(bp99)} | {fmt_delta(delta_pct(ap99, bp99))} "
            f"| {fmt_num(amean)} | {fmt_num(bmean)} | {fmt_delta(delta_pct(amean, bmean))} |"
        )
    out_path.write_text("\n".join(lines) + "\n")


PHASE_STAGES = {
    "P1 forwarder-pool-direct (target R3)":       ["R3", "R4"],
    "P2 parallel inval drain (target I6)":         ["I3", "I4", "I5", "I6", "I7", "I8"],
    "P3 hot-bucket sharding (target R1, wl-a)":    ["R1", "R2hit", "R2miss"],
    "P4 RCU cache_pool (target W10)":              ["W10", "W12"],
}


def write_consolidated(out_path, cells, all_data):
    """all_data[label] = (iter10a_rows, iter11a_rows)"""
    lines = []
    lines.append("# iter-10A → iter-11A consolidated path_decomp delta")
    lines.append("")
    lines.append("Per-Phase predicted-vs-actual gain across the 10 cells "
                 "(iter-10A Phase 4's best+worst per workload).")
    lines.append("")
    for phase_label, target_stages in PHASE_STAGES.items():
        lines.append(f"## {phase_label}")
        lines.append("")
        lines.append("| Cell | " + " | ".join(
            f"{s} p50 Δ% | {s} mean Δ%" for s in target_stages) + " |")
        lines.append("|---|" + "|".join(["---:"] * (len(target_stages) * 2)) + "|")
        for label, *_ in cells:
            a_rows, b_rows = all_data.get(label, ({}, {}))
            row = [label]
            for stage in target_stages:
                a = a_rows.get(stage, {})
                b = b_rows.get(stage, {})
                row.append(fmt_delta(delta_pct(a.get("p50"), b.get("p50"))))
                row.append(fmt_delta(delta_pct(a.get("mean"), b.get("mean"))))
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
        # Per-phase verdict: median p50 delta across cells where target stage had data
        avg_p50 = []
        avg_mean = []
        for label, *_ in cells:
            a_rows, b_rows = all_data.get(label, ({}, {}))
            for stage in target_stages:
                a = a_rows.get(stage, {}).get("p50")
                b = b_rows.get(stage, {}).get("p50")
                d = delta_pct(a, b)
                if d is not None:
                    avg_p50.append(d)
                a = a_rows.get(stage, {}).get("mean")
                b = b_rows.get(stage, {}).get("mean")
                d = delta_pct(a, b)
                if d is not None:
                    avg_mean.append(d)
        if avg_p50:
            median_p50 = sorted(avg_p50)[len(avg_p50) // 2]
            lines.append(f"**Median p50 Δ across all {len(avg_p50)} samples**: "
                         f"{fmt_delta(median_p50)}")
        if avg_mean:
            median_mean = sorted(avg_mean)[len(avg_mean) // 2]
            lines.append(f"**Median mean Δ across all {len(avg_mean)} samples**: "
                         f"{fmt_delta(median_mean)}")
        lines.append("")
    out_path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iter10a-dir", required=True)
    ap.add_argument("--iter11a-dir", required=True)
    ap.add_argument("--cells-file", required=True)
    args = ap.parse_args()

    i10a = Path(args.iter10a_dir)
    i11a = Path(args.iter11a_dir)
    cells = []
    with open(args.cells_file) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 6:
                continue
            cells.append(tuple(parts))

    all_data = {}
    for label, wl, kv, T, cache, expected in cells:
        i10a_path = i10a / "per_cell" / label / "per_stage_decomp.md"
        i11a_path = i11a / "per_cell" / label / "per_stage_decomp.md"
        i10a_rows = parse_per_stage_md(i10a_path)
        i11a_rows = parse_per_stage_md(i11a_path)
        all_data[label] = (i10a_rows, i11a_rows)
        out = i11a / "per_cell" / label / "iter10A_vs_iter11A_delta.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        write_per_cell_delta(out, label, wl, kv, T, cache, expected,
                             i10a_rows, i11a_rows)
        present = sum(1 for s in i11a_rows.values() if s.get("p50") is not None)
        print(f"  [{label}] iter11A parsed {present} healthy stages")

    consolidated = i11a / "consolidated_iter10A_to_iter11A_delta.md"
    write_consolidated(consolidated, cells, all_data)
    print(f"Wrote {consolidated}")


if __name__ == "__main__":
    main()
