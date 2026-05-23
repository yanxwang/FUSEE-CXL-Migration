#!/usr/bin/env python3
"""iter-18A xhost read decomp — parse PROBE_OP trace files (XRS*/XRR* tags),
compute per-op latencies, aggregate p50/p99/avg per stage.

Mirror of iter16A_xhost_decomp_analyze.py adapted to the read path probes
inserted by iter-18A Phase 1.

Usage:
  scripts/iter18A_read_decomp_analyze.py <h0_probe_dir> <h1_probe_dir> \\
      [--cpu-ghz 2.4] [--out <csv>]

Each probe dir contains files named like  probe.<pid>.<tid>  (per-thread,
mmap'd ring up to 512 MB). Header 16B + frames 24B (tag 8B, ns-field 8B,
op_id 8B). The "ns" field actually packs (cpu_id << 48) | (TSC_cycles &
0xFFFF_FFFF_FFFF).

Stages (xhost read):
  Worker (host h0 in convention) — 6 stages:
    Stage1 = XRS1S → XRS1E    slot_reserve  (ring->tail.fetch_add)
    Stage2 = XRS1E → XRS2E    slot_wait    (spin on slot free)
    Stage3 = XRS2E → XRS3E    req_publish  (write entry + flush + sfence)
    Stage4 = XRS3E → XRS4E    ack_wait     (spin on staging ready_op_id)
    Stage5 = XRS4E → XRS5E    cleanup_validate  (slot free + epoch/status checks)
    Stage6 = XRS5E → XRS6E    value_recv   (memcpy staging → caller / pool->read)
    Total  = XRS1S → XRS6E    (= StageW)

  Receiver (host h1) — 3 stages:
    StageR1 = XRR1S → XRR1E   ring_drain   (tail load + req_op_id read; gap-tolerance)
    StageR2 = XRR1E → XRR2E   handler      (bucket scan + dir lock + pool read + staging write)
    StageR3 = XRR2E → XRR3E   ack_publish  (resp_op_id store + flush)
    Total   = XRR1S → XRR3E   (= StageR)

  Cross-host:
    RTT = StageW (h0) − StageR (h1) per op_id pairing

Counters: XRS2R (slot_wait truly spun), XRS4T (ack_wait timeout),
          XRR1Z (gap encountered), XRR1X (gap exhausted).
"""

import os, sys, struct, argparse, glob
from collections import defaultdict

FRAME_SIZE = 24
HEADER_SIZE = 16
MAGIC = 0x424F5250  # "PROB"

WORKER_STAGES = [
    ("Stage1", "XRS1S", "XRS1E", "slot_reserve"),
    ("Stage2", "XRS1E", "XRS2E", "slot_wait"),
    ("Stage3", "XRS2E", "XRS3E", "req_publish"),
    ("Stage4", "XRS3E", "XRS4E", "ack_wait"),
    ("Stage5", "XRS4E", "XRS5E", "cleanup_validate"),
    ("Stage6", "XRS5E", "XRS6E", "value_recv"),
]
RECV_STAGES = [
    ("StageR1", "XRR1S", "XRR1E", "ring_drain"),
    ("StageR2", "XRR1E", "XRR2E", "handler"),
    ("StageR3", "XRR2E", "XRR3E", "ack_publish"),
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
    """Return dict[(pid, tid)] = list of (tag, cycles, op_id, cpu)."""
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
    """Pair XRS1S..XRS6E by op_id within a thread → per-op stage latencies."""
    ops = []
    counters = defaultdict(int)
    for (pid, tid), events in per_tid.items():
        bucket = defaultdict(dict)
        for tag, cycles, op_id, cpu in events:
            if tag == "XRS2R":
                # Payload is c_iters (slot-wait spin count), not op_id
                counters["XRS2R_total_spin"] += op_id
                counters["XRS2R_count"] += 1
                continue
            if tag == "XRS4T":
                counters["XRS4T_count"] += 1
                continue
            if tag.startswith("XRS"):
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
            if "XRS1S" in tagmap and "XRS6E" in tagmap:
                row["StageW"] = tagmap["XRS6E"] - tagmap["XRS1S"]
            else:
                ok = False
            row["complete"] = ok
            if ok:
                ops.append(row)
    return ops, counters


def extract_receiver_ops(per_tid):
    """Walk receiver events in order; XRR1S is start-of-drain (no op_id yet),
    XRR1E first attaches the op_id. We build per-ring-iteration records."""
    ops = []
    counters = defaultdict(int)
    for (pid, tid), events in per_tid.items():
        current = None
        for tag, cycles, op_id, cpu in events:
            if tag == "XRR1S":
                # XRR1S payload is `head` (current ring head), not op_id.
                current = {"pid": pid, "tid": tid, "XRR1S": cycles}
            elif tag == "XRR1Z":
                counters["XRR1Z_count"] += 1
            elif tag == "XRR1X":
                counters["XRR1X_count"] += 1
                current = None  # drain aborted; no op was processed
            elif tag == "XRR1E":
                if current is not None:
                    current["op_id"] = op_id
                    current["XRR1E"] = cycles
            elif tag == "XRR2E":
                if current is not None:
                    current["XRR2E"] = cycles
            elif tag == "XRR3E":
                if current is not None:
                    current["XRR3E"] = cycles
                    row = {"op_id": current.get("op_id"),
                           "pid": pid, "tid": tid}
                    ok = True
                    for stg, t0, t1, _ in RECV_STAGES:
                        if t0 in current and t1 in current:
                            row[stg] = current[t1] - current[t0]
                        else:
                            ok = False
                            break
                    if "XRR1S" in current and "XRR3E" in current:
                        row["StageR"] = current["XRR3E"] - current["XRR1S"]
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
                    help="CPU TSC frequency in GHz for cycles→ns (default 2.4 — g3/g4 Xeon)")
    ap.add_argument("--out", default="-", help="CSV output path (default stdout-only summary)")
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
    recv_ops,   r_counters = extract_receiver_ops(r_per_tid)

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
    if w_counters.get("XRS2R_count", 0) > 0:
        ratio = w_counters["XRS2R_count"] / total_w_ops * 100
        avg_spin = w_counters["XRS2R_total_spin"] / w_counters["XRS2R_count"]
        print(f"  XRS2R rate = {ratio:.4f}% of worker ops, avg spin iters = {avg_spin:.1f}")
    if w_counters.get("XRS4T_count", 0) > 0:
        ratio = w_counters["XRS4T_count"] / total_w_ops * 100
        print(f"  XRS4T (timeout) rate = {ratio:.4f}% of worker ops [INVESTIGATE if > 0]")
    for k, v in r_counters.items():
        print(f"  {k:<20s} = {v}")
    if r_counters.get("XRR1Z_count", 0) > 0:
        rate = r_counters["XRR1Z_count"] / total_r_ops * 100
        fail = r_counters.get("XRR1X_count", 0) / max(r_counters["XRR1Z_count"], 1) * 100
        print(f"  gap encounter rate = {rate:.4f}% of receiver ops")
        print(f"    exhausted = {fail:.1f}% of gaps")

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
