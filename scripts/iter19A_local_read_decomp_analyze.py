#!/usr/bin/env python3
"""iter-19A Phase 2.4 local_read decomp — parse PROBE_LR_OP trace files
(LRS1..LRS4 tags), pair ops by op_id, split HIT vs MISS by LRS2H/LRS2M
exit tag, compute per-stage medians.

Mirror of iter18A_read_decomp_analyze.py adapted to the local_read path
probes (no receiver — local_read is owner-self).

Stages (worker side only; local_read has no receiver):
  LRS1 entry        = LRS1S → LRS1E   key check + path entry
  LRS2 cache_lookup = LRS1E → LRS2[HM]  cache_pool seqlock CAS reader
                      LRS2H = HIT exit, LRS2M = MISS exit
  LRS3 cxl_miss     = LRS2M → LRS3E   bucket flush+scan + pool->read (MISS only)
                      (LRS3S marks miss-path begin; symmetric pair LRS3S→LRS3E
                       collapses to LRS2M→LRS3E end-to-end CXL stage time)
  LRS4 populate     = LRS4S → LRS4E   cache_pool_insert CAS (MISS only)

HIT path: LRS1 + LRS2 only
MISS path: LRS1 + LRS2 + LRS3 + LRS4

Usage:
  scripts/iter19A_local_read_decomp_analyze.py <probe_dir> [--cpu-ghz 2.4]

probe_dir contains probe.<pid>.<tid> files written by binary with
FUSEE_PROBE=1 -DFUSEE_LOCAL_READ_PROBE=1.
"""

import os, sys, struct, argparse, glob, statistics
from collections import defaultdict

FRAME_SIZE = 24
HEADER_SIZE = 16
MAGIC = 0x424F5250  # "PROB"


def parse_probe_file(path):
    """Read one probe file → list of (tag, cycles, op_id, cpu)."""
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
        cpu = (ns_field >> 48) & 0xFFFF
        events.append((tag, cycles, op_id, cpu))
    return events


def load_events(probe_dir):
    per_pidtid = defaultdict(list)
    files = glob.glob(os.path.join(probe_dir, "probe.*.*"))
    if not files:
        # Try alternate naming convention (e.g. /tmp/lr_probe_dump.PID.TID)
        files = glob.glob(probe_dir + ".*")
    for path in files:
        base = os.path.basename(path)
        parts = base.split(".")
        try:
            pid = int(parts[-2])
            tid = int(parts[-1])
        except (ValueError, IndexError):
            pid, tid = 0, hash(path)
        events = parse_probe_file(path)
        per_pidtid[(pid, tid)].extend(events)
    return per_pidtid


def extract_ops(per_tid):
    """Per-op stage latencies for HIT and MISS paths.

    Pair tags by op_id within a thread. Branch determined by LRS2H or
    LRS2M tag presence.
    """
    hit_ops = []
    miss_ops = []
    n_hit_partial = 0
    n_miss_partial = 0

    for (pid, tid), events in per_tid.items():
        bucket = defaultdict(dict)
        for tag, cycles, op_id, cpu in events:
            if tag.startswith("LRS"):
                # Tags: LRS1S, LRS1E, LRS2S, LRS2H, LRS2M,
                #       LRS3S, LRS3E, LRS4S, LRS4E
                bucket[op_id][tag] = cycles

        for op_id, tm in bucket.items():
            # Determine branch
            is_hit = "LRS2H" in tm
            is_miss = "LRS2M" in tm

            if is_hit and not is_miss:
                # HIT path: LRS1S → LRS1E → LRS2S → LRS2H
                if all(t in tm for t in ("LRS1S", "LRS1E", "LRS2S", "LRS2H")):
                    hit_ops.append({
                        "op_id": op_id,
                        "LRS1": tm["LRS1E"] - tm["LRS1S"],
                        "LRS2": tm["LRS2H"] - tm["LRS2S"],
                        "StageW": tm["LRS2H"] - tm["LRS1S"],
                    })
                else:
                    n_hit_partial += 1
            elif is_miss and not is_hit:
                # MISS path: LRS1S → LRS1E → LRS2S → LRS2M → LRS3S → LRS3E → LRS4S → LRS4E
                required = ("LRS1S", "LRS1E", "LRS2S", "LRS2M",
                            "LRS3S", "LRS3E", "LRS4S", "LRS4E")
                if all(t in tm for t in required):
                    miss_ops.append({
                        "op_id": op_id,
                        "LRS1": tm["LRS1E"] - tm["LRS1S"],
                        "LRS2": tm["LRS2M"] - tm["LRS2S"],
                        "LRS3": tm["LRS3E"] - tm["LRS3S"],
                        "LRS4": tm["LRS4E"] - tm["LRS4S"],
                        "StageW": tm["LRS4E"] - tm["LRS1S"],
                    })
                else:
                    n_miss_partial += 1
            else:
                # Ambiguous (both or neither) — skip
                n_miss_partial += 1
    return hit_ops, miss_ops, n_hit_partial, n_miss_partial


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
    ap.add_argument("probe_dir", help="dir containing probe.<pid>.<tid> files "
                                      "(or path prefix if dump is files-by-prefix)")
    ap.add_argument("--cpu-ghz", type=float, default=2.4,
                    help="CPU TSC frequency in GHz for cycles→ns (default 2.4)")
    ap.add_argument("--out", default="-", help="CSV output (default stdout summary)")
    args = ap.parse_args()

    print(f"# loading probe traces from {args.probe_dir}", file=sys.stderr)
    per_tid = load_events(args.probe_dir)
    n_events = sum(len(v) for v in per_tid.values())
    print(f"# threads: {len(per_tid)}, events: {n_events}", file=sys.stderr)

    hit_ops, miss_ops, hit_partial, miss_partial = extract_ops(per_tid)
    total = len(hit_ops) + len(miss_ops)
    hit_rate = len(hit_ops) / total * 100 if total else 0

    print()
    print("=== iter-19A local_read stage decomp ===")
    print()
    print(f"Total ops paired: {total}")
    print(f"  HIT  ops: {len(hit_ops):>10}  ({hit_rate:5.2f} %)")
    print(f"  MISS ops: {len(miss_ops):>10}  ({100-hit_rate:5.2f} %)")
    print(f"  HIT partial drops:  {hit_partial}")
    print(f"  MISS partial drops: {miss_partial}")
    print()
    print(f"CPU TSC frequency assumed: {args.cpu_ghz} GHz")
    print()

    # HIT path: LRS1 + LRS2
    print("─── HIT path (LRS1 + LRS2) ───")
    for stg in ("LRS1", "LRS2", "StageW"):
        s = stats([o[stg] for o in hit_ops], args.cpu_ghz)
        print(f"{stg:>8}: {fmt(s)}")
    print()

    # MISS path: LRS1 + LRS2 + LRS3 + LRS4
    print("─── MISS path (LRS1 + LRS2 + LRS3 + LRS4) ───")
    for stg in ("LRS1", "LRS2", "LRS3", "LRS4", "StageW"):
        s = stats([o[stg] for o in miss_ops], args.cpu_ghz)
        print(f"{stg:>8}: {fmt(s)}")
    print()

    # CSV out if requested
    if args.out != "-":
        with open(args.out, "w") as f:
            f.write("path,stage,n,p50_ns,p99_ns,avg_ns\n")
            for stg in ("LRS1", "LRS2", "StageW"):
                s = stats([o[stg] for o in hit_ops], args.cpu_ghz)
                f.write(f"HIT,{stg},{s['n']},{s['p50_ns']:.1f},{s['p99_ns']:.1f},{s['avg_ns']:.1f}\n")
            for stg in ("LRS1", "LRS2", "LRS3", "LRS4", "StageW"):
                s = stats([o[stg] for o in miss_ops], args.cpu_ghz)
                f.write(f"MISS,{stg},{s['n']},{s['p50_ns']:.1f},{s['p99_ns']:.1f},{s['avg_ns']:.1f}\n")
        print(f"# CSV → {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
