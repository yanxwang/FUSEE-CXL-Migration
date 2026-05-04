#!/usr/bin/env python3
"""iter-8A spare-time parser v3: per-thread time-adjacent stage attribution
on the post-fix RDTSCP-packed probe format.

Frame ns field = (TSC_cycles & 0xFFFFFFFFFFFF) | ((cpu_id & 0xFFFF) << 48)

This parser groups by thread (file path), sorts by TSC, and computes
deltas between consecutive frames to avoid the iter-7A op_id-grouping
bug (workload-d reuses keys → cross-op contamination).

It also reports max-tail samples with surrounding context so the
specific spinlock/timeout that caused the residual collapse can be
named (not just classified).
"""
import os
import struct
import sys
import statistics
from collections import defaultdict
from pathlib import Path

TSC_HZ = 2_000_225_428  # calibrated on g3
NS_PER_CYCLE = 1e9 / TSC_HZ  # ~0.4999

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
    print(f"# {len(files)} probe files")

    adjacent = defaultdict(list)
    longest_per_stage = defaultdict(list)  # keep top-5 max samples + their op_ids/cpus

    total_frames = 0
    for f in files:
        frames = parse_dump(f)
        total_frames += len(frames)
        if len(frames) < 2:
            continue
        # Sort by ns within thread (TSC is monotonic per-cpu but might not be perfectly
        # ordered if frames migrate; LFENCE helps here)
        frames.sort(key=lambda x: x[1])
        for i in range(1, len(frames)):
            prev = frames[i - 1]
            cur = frames[i]
            delta_ns = cur[1] - prev[1]
            if delta_ns < 0 or delta_ns > 60e9:  # >60s is impossible single-stage
                continue
            stage = f"{prev[0]}->{cur[0]}"
            adjacent[stage].append(delta_ns)
            # Track top-5 max with context
            top = longest_per_stage[stage]
            if len(top) < 5 or delta_ns > top[-1][0]:
                top.append((delta_ns, prev[2], cur[2], prev[3], cur[3], str(f.name)))
                top.sort(key=lambda x: -x[0])
                if len(top) > 5:
                    top.pop()

    print(f"# {total_frames} total frames")

    rows = []
    for stage, deltas in sorted(adjacent.items()):
        if len(deltas) < 5:
            continue
        rows.append({
            "stage": stage,
            "n": len(deltas),
            "p50": percentile(deltas, 0.50) / 1000.0,
            "p90": percentile(deltas, 0.90) / 1000.0,
            "p99": percentile(deltas, 0.99) / 1000.0,
            "max": max(deltas) / 1000.0,
            "mean": statistics.mean(deltas) / 1000.0,
        })
    rows.sort(key=lambda r: -r["max"])

    out = []
    out.append("# Stage transitions (per-thread time-adjacent, post-fix residual collapse)")
    out.append("")
    out.append("| Stage transition | N | p50 µs | p90 µs | p99 µs | max µs | mean µs |")
    out.append("|---|---|---|---|---|---|---|")
    for r in rows:
        out.append(f"| {r['stage']} | {r['n']} | {r['p50']:.2f} | "
                   f"{r['p90']:.2f} | {r['p99']:.2f} | {r['max']:.2f} | {r['mean']:.2f} |")

    out.append("")
    out.append("## Top-5 max tails per dominant stage (context: prev_op_id, cur_op_id, prev_cpu, cur_cpu, file)")
    out.append("")
    # Show top-5 for stages whose max > 1ms (the interesting tails)
    for r in rows[:10]:
        stage = r["stage"]
        if r["max"] < 1000.0:
            continue
        top = longest_per_stage[stage]
        out.append(f"### {stage}  (max {r['max']:.2f} µs)")
        out.append("| rank | delta µs | prev op_id | cur op_id | prev cpu | cur cpu | file |")
        out.append("|---|---|---|---|---|---|---|")
        for i, t in enumerate(top, 1):
            out.append(f"| {i} | {t[0]/1000.0:.2f} | {t[1]} | {t[2]} | {t[3]} | {t[4]} | {t[5]} |")
        out.append("")

    text = "\n".join(out)
    if out_path:
        Path(out_path).write_text(text + "\n")
        print(f"wrote {out_path}")
    else:
        print(text)


if __name__ == "__main__":
    main()
