#!/usr/bin/env python3
"""iter-7A Phase 2: probe anomaly scan.

Identify (a) longest gaps between consecutive frames in any thread
(catches "thread stalled here for X ms") and (b) slowest 1% of stage
transitions (with op_id linkage so caller can drill into the
specific request).
"""
import os
import struct
import sys
from collections import defaultdict
from pathlib import Path


def parse_dump(path):
    frames = []
    try:
        with open(path, "rb") as f:
            hdr = f.read(16)
            if len(hdr) < 16: return frames
            magic, capacity, count = struct.unpack("<IIQ", hdr)
            if magic != 0x424F5250: return frames
            for _ in range(count):
                buf = f.read(24)
                if len(buf) < 24: break
                tag = buf[:8].split(b"\x00", 1)[0].decode("ascii", errors="replace")
                ns, op_id = struct.unpack("<QQ", buf[8:24])
                frames.append((tag, ns, op_id))
    except Exception:
        pass
    return frames


def main():
    dump_dir = Path(sys.argv[1])
    files = sorted(dump_dir.glob("probe*"))
    print(f"# scanning {len(files)} probe dumps in {dump_dir}")

    print("\n## Top-15 longest intra-thread gaps (thread stalls)")
    print("| Dump file | Gap ms | After tag | Before tag | After op_id |")
    print("|---|---|---|---|---|")
    all_gaps = []
    for f in files:
        frames = parse_dump(f)
        for i in range(1, len(frames)):
            gap = frames[i][1] - frames[i-1][1]
            all_gaps.append((gap, f.name, frames[i-1][0], frames[i][0], frames[i-1][2]))
    all_gaps.sort(reverse=True)
    for g, fn, t0, t1, op in all_gaps[:15]:
        print(f"| {fn} | {g/1e6:.2f} | {t0} | {t1} | {op:#x} |")

    by_op = defaultdict(list)
    for f in files:
        for tag, ns, op_id in parse_dump(f):
            by_op[op_id].append((ns, tag, f.name))
    stage_deltas = defaultdict(list)
    for op_id, frames in by_op.items():
        if op_id == 0: continue
        frames.sort()
        anchor_ns, anchor_tag, _ = frames[0]
        for ns, tag, _ in frames:
            if tag == anchor_tag: continue
            stage_deltas[f"{anchor_tag}->{tag}"].append((ns - anchor_ns, op_id))

    print("\n## Top-5 slowest 1% per stage (with op_id)")
    print("| Stage | N | p50 µs | p99 µs | top-3 max ms (op_id) |")
    print("|---|---|---|---|---|")
    for stage, items in sorted(stage_deltas.items()):
        if len(items) < 100: continue
        items.sort()
        p50 = items[len(items) // 2][0]
        p99 = items[int(len(items) * 0.99)][0]
        top3 = items[-3:]
        top3_str = "; ".join(f"{d/1e6:.2f}({op:#x})" for d, op in top3)
        print(f"| {stage} | {len(items)} | {p50/1000:.2f} | {p99/1000:.2f} | {top3_str} |")


if __name__ == "__main__":
    main()
