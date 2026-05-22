#!/usr/bin/env python3
"""iter-16A xhost write decomp — parse PROBE_OP trace files, compute per-op
latencies, aggregate p50/p99/avg per stage.

Usage:
  scripts/iter16A_xhost_decomp_analyze.py <h0_probe_dir> <h1_probe_dir> \\
      [--cpu-ghz 2.4] [--out <csv>]

Each probe dir contains files named like  probe.<pid>.<tid>  (one per thread,
128MB mmap'd ring). Header 16B + frames 24B (tag 8B, ns-field 8B, op_id 8B).
The "ns" field actually packs (cpu_id << 48) | (TSC_cycles & 0xFFFF_FFFF_FFFF).

Stages (xhost write):
  Worker (host h0 in convention):
    Stage 1 = XWS1S → XWS1E    slot_reserve
    Stage 2 = XWS1E → XWS2E    slot_wait
    Stage 3 = XWS2E → XWS3E    value_xfer
    Stage 4 = XWS3E → XWS4E    ctrl_publish
    Stage 5 = XWS4E → XWS5E    ack_wait
    Total   = XWS1S → XWS5E    (= XWStageW)
  Receiver (host h1):
    Stage 6 = XWR6S → XWR6E    rcv_poll
    Stage 7 = XWR6E → XWR7E    rcv_work
    Stage 8 = XWR7E → XWR8E    ack_publish
    Total   = XWR6S → XWR8E    (= XWStageR)
  Cross-host:
    XWRTT   = XWStageW (h0) − XWStageR (h1) per op_id pairing

Counters: XWS2R (C truly spun), XWS5T (timeout), XWR6Z/H/X (gap subsystem).
"""

import os, sys, struct, argparse, glob, statistics
from collections import defaultdict

FRAME_SIZE = 24
HEADER_SIZE = 16
MAGIC = 0x424F5250  # "PROB"

WORKER_STAGES = [
    ("Stage1", "XWS1S", "XWS1E", "slot_reserve"),
    ("Stage2", "XWS1E", "XWS2E", "slot_wait"),
    ("Stage3", "XWS2E", "XWS3E", "value_xfer"),
    ("Stage4", "XWS3E", "XWS4E", "ctrl_publish"),
    ("Stage5", "XWS4E", "XWS5E", "ack_wait"),
]
RECV_STAGES = [
    ("Stage6", "XWR6S", "XWR6E", "rcv_poll"),
    ("Stage7", "XWR6E", "XWR7E", "rcv_work"),
    ("Stage8", "XWR7E", "XWR8E", "ack_publish"),
]


def parse_probe_file(path):
    """Read one probe.<pid>.<tid> file → list of (tag, cycles, op_id, cpu)."""
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


def load_host_events(probe_dir):
    """Return dict[(pid, tid)] = list of (tag, cycles, op_id, cpu).

    Important: must key on (pid, tid) NOT tid alone — child workers after fork
    can have identical tid numbers (each process numbers threads independently),
    so keying on tid would mix events from different worker processes.
    """
    per_pidtid = defaultdict(list)
    files = glob.glob(os.path.join(probe_dir, "probe.*.*"))
    if not files:
        files = glob.glob(os.path.join(probe_dir, "*"))
        files = [f for f in files if os.path.basename(f).startswith("probe.")]
    for path in files:
        base = os.path.basename(path)
        parts = base.split(".")
        try:
            pid = int(parts[1])
            tid = int(parts[2]) if len(parts) > 2 else 0
        except (ValueError, IndexError):
            pid, tid = 0, hash(path)
        events = parse_probe_file(path)
        per_pidtid[(pid, tid)].extend(events)
    return per_pidtid


def extract_worker_ops(per_tid):
    """For each thread, pair XWS1S..XWS5E by op_id → per-op latencies."""
    ops = []  # list of dicts
    counters = defaultdict(int)
    for (pid, tid), events in per_tid.items():
        # Group by op_id within this thread.
        bucket = defaultdict(dict)  # op_id -> {tag: cycles}
        for tag, cycles, op_id, cpu in events:
            if tag == "XWS2R":
                # Payload is c_iters, not op_id — separate counter
                counters["XWS2R_total_spin"] += op_id  # op_id field holds count
                counters["XWS2R_count"] += 1
                continue
            if tag == "XWS5T":
                counters["XWS5T_count"] += 1
                continue
            if tag.startswith("XWS"):
                bucket[op_id][tag] = cycles
        for op_id, tagmap in bucket.items():
            row = {"op_id": op_id, "pid": pid, "tid": tid}
            ok = True
            for stg, t0, t1, _ in WORKER_STAGES:
                if t0 in tagmap and t1 in tagmap:
                    row[stg] = tagmap[t1] - tagmap[t0]
                else:
                    ok = False
                    break
            if "XWS1S" in tagmap and "XWS5E" in tagmap:
                row["StageW"] = tagmap["XWS5E"] - tagmap["XWS1S"]
            else:
                ok = False
            row["complete"] = ok
            if ok:
                ops.append(row)
    return ops, counters


def extract_receiver_ops(per_tid):
    """For each thread, walk events in order: pair XWR6S → ... → XWR8E. Match op_id from XWR6E onwards."""
    ops = []
    counters = defaultdict(int)
    for (pid, tid), events in per_tid.items():
        # Walk events in order; build per-op record on the fly
        current = None  # in-progress receiver op record
        for tag, cycles, op_id, cpu in events:
            if tag == "XWR6S":
                # Start new op record
                current = {"pid": pid, "tid": tid, "XWR6S": cycles}
            elif tag == "XWR6Z":
                counters["XWR6Z_count"] += 1
            elif tag == "XWR6H":
                counters["XWR6H_count"] += 1
            elif tag == "XWR6X":
                counters["XWR6X_count"] += 1
                current = None  # abandon: gap exhausted, no further events
            elif tag == "XWR6E":
                if current is not None:
                    current["op_id"] = op_id
                    current["XWR6E"] = cycles
            elif tag == "XWR7E":
                if current is not None:
                    current["XWR7E"] = cycles
            elif tag == "XWR8E":
                if current is not None:
                    current["XWR8E"] = cycles
                    # finalize
                    row = {
                        "op_id": current.get("op_id"),
                        "pid": pid,
                        "tid": tid,
                    }
                    ok = True
                    for stg, t0, t1, _ in RECV_STAGES:
                        if t0 in current and t1 in current:
                            row[stg] = current[t1] - current[t0]
                        else:
                            ok = False
                            break
                    if "XWR6S" in current and "XWR8E" in current:
                        row["StageR"] = current["XWR8E"] - current["XWR6S"]
                    else:
                        ok = False
                    if ok and row["op_id"]:
                        ops.append(row)
                    current = None
    return ops, counters


def stats(values, cpu_ghz):
    if not values:
        return {"n": 0}
    vs = sorted(values)
    n = len(vs)
    p50 = vs[n // 2]
    p99 = vs[min(n - 1, int(n * 0.99))]
    avg = sum(vs) / n
    cyc_to_ns = (lambda c: c / cpu_ghz) if cpu_ghz else (lambda c: c)
    return {
        "n": n,
        "p50_cyc": p50, "p50_ns": cyc_to_ns(p50),
        "p99_cyc": p99, "p99_ns": cyc_to_ns(p99),
        "avg_cyc": avg, "avg_ns": cyc_to_ns(avg),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("h0_probe_dir", help="dir containing host 0 (worker) probe.*.* files")
    ap.add_argument("h1_probe_dir", help="dir containing host 1 (receiver) probe.*.* files")
    ap.add_argument("--cpu-ghz", type=float, default=2.4,
                    help="CPU TSC frequency in GHz for cycles→ns (default 2.4)")
    ap.add_argument("--out", default="-", help="CSV output path (default stdout)")
    args = ap.parse_args()

    print(f"# loading worker traces from {args.h0_probe_dir}", file=sys.stderr)
    w_per_tid = load_host_events(args.h0_probe_dir)
    print(f"# loading receiver traces from {args.h1_probe_dir}", file=sys.stderr)
    r_per_tid = load_host_events(args.h1_probe_dir)

    print(f"# worker threads: {len(w_per_tid)}, total events: "
          f"{sum(len(v) for v in w_per_tid.values())}", file=sys.stderr)
    print(f"# receiver threads: {len(r_per_tid)}, total events: "
          f"{sum(len(v) for v in r_per_tid.values())}", file=sys.stderr)

    worker_ops, w_counters = extract_worker_ops(w_per_tid)
    recv_ops, r_counters = extract_receiver_ops(r_per_tid)

    print(f"# worker complete ops: {len(worker_ops)}", file=sys.stderr)
    print(f"# receiver complete ops: {len(recv_ops)}", file=sys.stderr)

    # Pair by op_id
    recv_by_op = {r["op_id"]: r for r in recv_ops}
    pairs = []
    for w in worker_ops:
        r = recv_by_op.get(w["op_id"])
        if r:
            row = dict(w)
            for stg, _, _, _ in RECV_STAGES:
                row[stg] = r.get(stg)
            row["StageR"] = r.get("StageR")
            if row.get("StageW") is not None and row.get("StageR") is not None:
                row["RTT"] = row["StageW"] - row["StageR"]
            pairs.append(row)

    print(f"# w↔r paired ops: {len(pairs)}", file=sys.stderr)

    # Aggregates
    print("\n=== Aggregates (median of N paired ops) ===")
    print(f"{'metric':<10s}  {'n':>8s}  {'p50_cyc':>10s}  {'p50_ns':>10s}  {'p99_ns':>10s}  {'avg_ns':>10s}")
    all_metrics = [s[0] for s in WORKER_STAGES] + ["StageW"] + [s[0] for s in RECV_STAGES] + ["StageR", "RTT"]
    for m in all_metrics:
        vals = [p[m] for p in pairs if p.get(m) is not None]
        st = stats(vals, args.cpu_ghz)
        if st["n"] > 0:
            print(f"{m:<10s}  {st['n']:>8d}  {st['p50_cyc']:>10.0f}  "
                  f"{st['p50_ns']:>10.1f}  {st['p99_ns']:>10.1f}  {st['avg_ns']:>10.1f}")

    print("\n=== Event counters ===")
    total_w_ops = len(worker_ops) or 1
    total_r_ops = len(recv_ops) or 1
    for k, v in w_counters.items():
        print(f"  {k:<20s} = {v}")
    if w_counters.get("XWS2R_count", 0) > 0:
        ratio = w_counters["XWS2R_count"] / total_w_ops * 100
        avg_spin = w_counters["XWS2R_total_spin"] / w_counters["XWS2R_count"]
        print(f"  XWS2R rate = {ratio:.4f}% of worker ops, avg spin iters = {avg_spin:.1f}")
    if w_counters.get("XWS5T_count", 0) > 0:
        ratio = w_counters["XWS5T_count"] / total_w_ops * 100
        print(f"  XWS5T (timeout) rate = {ratio:.4f}% of worker ops [INVESTIGATE if > 0]")
    for k, v in r_counters.items():
        print(f"  {k:<20s} = {v}")
    if r_counters.get("XWR6Z_count", 0) > 0:
        rate = r_counters["XWR6Z_count"] / total_r_ops * 100
        heal = r_counters.get("XWR6H_count", 0) / r_counters["XWR6Z_count"] * 100
        fail = r_counters.get("XWR6X_count", 0) / r_counters["XWR6Z_count"] * 100
        print(f"  gap encounter rate = {rate:.4f}% of receiver ops")
        print(f"    healed   = {heal:.1f}% of gaps")
        print(f"    exhausted = {fail:.1f}% of gaps")

    # CSV dump (per-op)
    if args.out and args.out != "-":
        with open(args.out, "w") as f:
            cols = ["op_id"] + [s[0] for s in WORKER_STAGES] + ["StageW"] + \
                   [s[0] for s in RECV_STAGES] + ["StageR", "RTT"]
            f.write(",".join(cols) + "\n")
            for p in pairs:
                f.write(",".join(str(p.get(c, "")) for c in cols) + "\n")
        print(f"\nwrote per-op CSV: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
