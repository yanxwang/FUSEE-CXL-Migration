#!/usr/bin/env python3
"""iter-20A local_write decomp — parse PROBE_LW_OP trace files
(LWS1..LWS6 tags), pair ops by op_id, compute per-stage medians.

Stages:
  LWS1 entry+slotscan = LWS1S → LWS1E
  LWS2 slot_dir_lock  = LWS2S → LWS2E
  LWS3 sharer_inval   = LWS3S → LWS3E   (LWS3B branch: broadcast taken)
  LWS4 cow_publish    = LWS4S → LWS4E   (LWS4A, LWS4W sub-anchors for blockpool path)
  LWS5 dir_state      = LWS5S → LWS5E
  LWS6 own_cache      = LWS6S → LWS6E
  StageW              = LWS6E − LWS1S   (full local_write)

Sub-stage breakdown for blockpool path (when LWS4A and LWS4W present):
  LWS4_alloc   = LWS4A − LWS4S
  LWS4_write   = LWS4W − LWS4A
  LWS4_publish = LWS4E − LWS4W

Usage:
  scripts/iter20A_local_write_decomp_analyze.py <probe_dir> [--cpu-ghz 2.4]
"""

import os, sys, struct, argparse, glob
from collections import defaultdict

FRAME_SIZE = 24
HEADER_SIZE = 16
MAGIC = 0x424F5250  # "PROB"


def parse_probe_file(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if len(data) < HEADER_SIZE:
        return []
    magic, capacity, count = struct.unpack_from("<IIQ", data, 0)
    if magic != MAGIC:
        return []
    events = []
    end = HEADER_SIZE + count * FRAME_SIZE
    if end > len(data):
        end = len(data)
    for off in range(HEADER_SIZE, end, FRAME_SIZE):
        tag_raw, ns_field, op_id = struct.unpack_from("<8sQQ", data, off)
        tag = tag_raw.rstrip(b"\x00").decode("ascii", errors="ignore")
        cycles = ns_field & 0x0000_FFFF_FFFF_FFFF
        events.append((tag, cycles, op_id))
    return events


def load_events(probe_dir):
    per_tid = defaultdict(list)
    files = glob.glob(os.path.join(probe_dir, "probe.*.*"))
    for path in files:
        base = os.path.basename(path)
        parts = base.split(".")
        try:
            pid = int(parts[-2])
            tid = int(parts[-1])
        except (ValueError, IndexError):
            pid, tid = 0, hash(path)
        per_tid[(pid, tid)].extend(parse_probe_file(path))
    return per_tid


def extract_ops(per_tid):
    full_ops = []        # success path: LWS1..LWS6 all present
    blockpool_subs = []  # ops with LWS4A + LWS4W (blockpool sub-stage timing)
    n_partial = 0

    for (pid, tid), events in per_tid.items():
        bucket = defaultdict(dict)
        for tag, cycles, op_id in events:
            if tag.startswith("LWS"):
                bucket[op_id][tag] = cycles

        required = ("LWS1S", "LWS1E", "LWS2S", "LWS2E",
                    "LWS3S", "LWS3E",
                    "LWS4S", "LWS4E",
                    "LWS5S", "LWS5E",
                    "LWS6S", "LWS6E")
        for op_id, tm in bucket.items():
            if not all(t in tm for t in required):
                n_partial += 1
                continue
            row = {
                "op_id": op_id,
                "LWS1": tm["LWS1E"] - tm["LWS1S"],
                "LWS2": tm["LWS2E"] - tm["LWS2S"],
                "LWS3": tm["LWS3E"] - tm["LWS3S"],
                "LWS4": tm["LWS4E"] - tm["LWS4S"],
                "LWS5": tm["LWS5E"] - tm["LWS5S"],
                "LWS6": tm["LWS6E"] - tm["LWS6S"],
                "StageW": tm["LWS6E"] - tm["LWS1S"],
            }
            full_ops.append(row)
            # If blockpool path (LWS4A + LWS4W present), record sub-timing.
            if "LWS4A" in tm and "LWS4W" in tm:
                blockpool_subs.append({
                    "alloc":   tm["LWS4A"] - tm["LWS4S"],
                    "write":   tm["LWS4W"] - tm["LWS4A"],
                    "publish": tm["LWS4E"] - tm["LWS4W"],
                })
    return full_ops, blockpool_subs, n_partial


def stats(values, cpu_ghz):
    if not values:
        return {"n": 0, "p50_ns": 0, "p99_ns": 0, "avg_ns": 0}
    vs = sorted(values)
    n = len(vs)
    p50 = vs[n // 2]
    p99 = vs[min(n - 1, int(n * 0.99))]
    avg = sum(vs) / n
    cyc_to_ns = (lambda c: c / cpu_ghz) if cpu_ghz else (lambda c: c)
    return {
        "n": n,
        "p50_ns": cyc_to_ns(p50),
        "p99_ns": cyc_to_ns(p99),
        "avg_ns": cyc_to_ns(avg),
    }


def fmt(s):
    if s["n"] == 0:
        return "  (no data)"
    return f"  n={s['n']:>10}  p50={s['p50_ns']:>10.1f} ns  p99={s['p99_ns']:>10.1f} ns  avg={s['avg_ns']:>10.1f} ns"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probe_dir")
    ap.add_argument("--cpu-ghz", type=float, default=2.4)
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    print(f"# loading probe traces from {args.probe_dir}", file=sys.stderr)
    per_tid = load_events(args.probe_dir)
    n_events = sum(len(v) for v in per_tid.values())
    print(f"# threads: {len(per_tid)}, events: {n_events}", file=sys.stderr)

    full_ops, blockpool_subs, n_partial = extract_ops(per_tid)
    print()
    print("=== iter-20A local_write stage decomp ===")
    print()
    print(f"Total full-path ops: {len(full_ops)}")
    print(f"Blockpool sub-timing ops: {len(blockpool_subs)}")
    print(f"Partial drops: {n_partial}")
    print()
    print(f"CPU TSC frequency assumed: {args.cpu_ghz} GHz")
    print()

    print("─── LWS1..6 + StageW ───")
    for stg in ("LWS1", "LWS2", "LWS3", "LWS4", "LWS5", "LWS6", "StageW"):
        s = stats([o[stg] for o in full_ops], args.cpu_ghz)
        print(f"{stg:>8}: {fmt(s)}")
    print()
    if blockpool_subs:
        print("─── LWS4 sub-stages (blockpool path) ───")
        for stg in ("alloc", "write", "publish"):
            s = stats([o[stg] for o in blockpool_subs], args.cpu_ghz)
            print(f"  LWS4_{stg:<8}: {fmt(s)}")


if __name__ == "__main__":
    main()
