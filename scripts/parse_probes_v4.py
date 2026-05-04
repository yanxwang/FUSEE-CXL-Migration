#!/usr/bin/env python3
"""iter-8A Phase 3 parser v4: per-STAGE statistics (not per-transition).

For each stage S, compute the time spent IN S = time of NEXT probe
(on same thread) - time of PROBE(S). This gives the actual
"how long did stage S take before moving on" duration.

Usage: parse_probes_v4.py <dump_dir> [--out FILE]
"""
import os
import struct
import sys
import statistics
from collections import defaultdict
from pathlib import Path

TSC_HZ = 2_000_225_428
NS_PER_CYCLE = 1e9 / TSC_HZ
CYCLES_MASK = 0x0000FFFFFFFFFFFF


def parse_dump(path):
    out = []
    try:
        with open(path, "rb") as f:
            hdr = f.read(16)
            if len(hdr) < 16:
                return out
            magic, capacity, count = struct.unpack("<IIQ", hdr)
            if magic != 0x424F5250:
                return out
            for _ in range(count):
                buf = f.read(24)
                if len(buf) < 24:
                    break
                tag = buf[:8].split(b"\x00", 1)[0].decode("ascii", errors="replace")
                packed_ns, op_id = struct.unpack("<QQ", buf[8:24])
                cycles = packed_ns & CYCLES_MASK
                cpu_id = (packed_ns >> 48) & 0xFFFF
                ns = cycles * NS_PER_CYCLE
                out.append((tag, ns, op_id, cpu_id))
    except Exception as e:
        print(f"parse error {path}: {e}", file=sys.stderr)
    return out


def percentile(xs, q):
    if not xs:
        return 0
    xs_sorted = sorted(xs)
    return xs_sorted[min(len(xs_sorted) - 1, int(q * len(xs_sorted)))]


def main():
    dump_dir = Path(sys.argv[1])
    out_path = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    files = sorted(dump_dir.rglob("probe.*.*"))
    print(f"# {len(files)} probe files", file=sys.stderr)

    per_stage = defaultdict(list)  # tag → list of duration ns
    total_frames = 0
    for f in files:
        frames = parse_dump(f)
        total_frames += len(frames)
        if len(frames) < 2:
            continue
        frames.sort(key=lambda x: x[1])
        for i in range(len(frames) - 1):
            tag = frames[i][0]
            duration_ns = frames[i + 1][1] - frames[i][1]
            if duration_ns < 0 or duration_ns > 60e9:
                continue
            per_stage[tag].append(duration_ns)
    print(f"# {total_frames} total frames", file=sys.stderr)

    rows = []
    for stage, durs in per_stage.items():
        if len(durs) < 5:
            continue
        rows.append({
            "stage": stage,
            "n": len(durs),
            "p50": percentile(durs, 0.50) / 1000.0,
            "p90": percentile(durs, 0.90) / 1000.0,
            "p99": percentile(durs, 0.99) / 1000.0,
            "max": max(durs) / 1000.0,
            "mean": statistics.mean(durs) / 1000.0,
        })

    # Sort by canonical stage order if known
    stage_order = ["W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8", "W9",
                   "W10", "W11", "W12",
                   "R1", "R2hit", "R2miss", "R3", "R4", "R5", "R6",
                   "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8",
                   "F1", "F2", "F3", "F4", "F5", "F6", "F7",
                   "D1", "D2", "D3", "D4", "D5"]
    order_map = {t: i for i, t in enumerate(stage_order)}
    rows.sort(key=lambda r: (order_map.get(r["stage"], 999), r["stage"]))

    out = []
    out.append("| Stage | N | p50 µs | p90 µs | p99 µs | max µs | mean µs |")
    out.append("|---|---|---|---|---|---|---|")
    for r in rows:
        out.append(f"| {r['stage']} | {r['n']} | {r['p50']:.3f} | "
                   f"{r['p90']:.3f} | {r['p99']:.3f} | {r['max']:.3f} | {r['mean']:.3f} |")

    text = "\n".join(out)
    if out_path:
        Path(out_path).write_text(text + "\n")
        print(f"wrote {out_path}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
