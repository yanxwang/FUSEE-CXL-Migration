#!/usr/bin/env python3
"""iter-7A Phase 2: probe parser for persistent mmap dump format.

Header (16 B): magic ("PROB"=0x424F5250) + capacity (u32) + count (u64)
Frames (24 B each): tag[8] + ns (u64) + op_id (u64)
"""
import os
import struct
import sys
import statistics
from collections import defaultdict
from pathlib import Path


def parse_dump(path):
    frames = []
    try:
        with open(path, "rb") as f:
            hdr = f.read(16)
            if len(hdr) < 16: return frames
            magic, capacity, count = struct.unpack("<IIQ", hdr)
            if magic != 0x424F5250:
                return frames
            for _ in range(count):
                buf = f.read(24)
                if len(buf) < 24: break
                tag = buf[:8].split(b"\x00", 1)[0].decode("ascii", errors="replace")
                ns, op_id = struct.unpack("<QQ", buf[8:24])
                frames.append((tag, ns, op_id))
    except Exception as e:
        print(f"parse error {path}: {e}", file=sys.stderr)
    return frames


def percentile(xs, q):
    if not xs: return 0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def main():
    dump_dir = Path(sys.argv[1])
    out_path = None
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    all_frames = []
    files = sorted(dump_dir.glob("probe*"))
    for f in files:
        for tag, ns, op_id in parse_dump(f):
            all_frames.append((tag, ns, op_id, f.name))
    print(f"# parsed {len(all_frames)} frames from {len(files)} dumps")
    by_op = defaultdict(list)
    for tag, ns, op_id, src in all_frames:
        by_op[op_id].append((ns, tag, src))
    stage_deltas = defaultdict(list)
    for op_id, frames in by_op.items():
        if op_id == 0: continue
        frames.sort()
        anchor_ns, anchor_tag, _ = frames[0]
        for ns, tag, _ in frames:
            if tag == anchor_tag: continue
            delta = ns - anchor_ns
            stage_deltas[f"{anchor_tag}->{tag}"].append(delta)
    rows = []
    for stage, deltas in sorted(stage_deltas.items()):
        if len(deltas) < 5: continue
        rows.append({
            "stage": stage,
            "n": len(deltas),
            "p50_us": percentile(deltas, 0.50) / 1000.0,
            "p90_us": percentile(deltas, 0.90) / 1000.0,
            "p99_us": percentile(deltas, 0.99) / 1000.0,
            "max_us": max(deltas) / 1000.0,
            "mean_us": statistics.mean(deltas) / 1000.0,
        })
    rows.sort(key=lambda r: -r["p99_us"])
    out = []
    out.append("| Stage transition | N | p50 µs | p90 µs | p99 µs | max µs | mean µs |")
    out.append("|---|---|---|---|---|---|---|")
    for r in rows:
        out.append(f"| {r['stage']} | {r['n']} | {r['p50_us']:.2f} | "
                   f"{r['p90_us']:.2f} | {r['p99_us']:.2f} | "
                   f"{r['max_us']:.2f} | {r['mean_us']:.2f} |")
    text = "\n".join(out)
    if out_path:
        Path(out_path).write_text(text + "\n")
        print(f"wrote {out_path}")
    else:
        print(text)


if __name__ == "__main__":
    main()
