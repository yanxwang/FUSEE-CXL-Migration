#!/usr/bin/env python3
"""iter-12A Phase 5.1 post-process: read per-thread probe files, cross-
correlate worker + receiver events by op_id, output per-op timeline +
summary statistics.

Probe file format (cxl_probe.h):
  - 16 B header: u32 magic="PROB" + u32 capacity + u64 count
  - N × 24 B frames: char[8] tag + u64 ts_packed + u64 op_id
    ts_packed: low 48 bits = TSC cycles, high 16 bits = CPU id

op_id encoding (encode_op_id):
  - high byte = (host_id + 1)
  - low 56 bits = sequence number

Phase 5 tags:
  Worker side (in forward_write_direct on the OP-ISSUING host):
    P5W_FA   = before fetch_add(tail)
    P5W_SR   = after fetch_add, before slot-free wait
    P5W_SF   = after slot-free wait, before memcpy
    P5W_PP   = before publish (req_op_id store)
    P5W_PT   = after publish (post store_fence)
    P5W_OK = generic_spin_wait returned success (resp received)
    P5W_TO = generic_spin_wait returned -11 (5ms timeout)

  Receiver side (in write_receiver_loop on the OWNER host):
    P5R_GZ   = first read req_op_id was zero (gap encounter; tag-op_id is `head` not real op_id)
    P5R_GH   = gap healed during budget spin (op_id is real)
    P5R_GX   = gap budget exhausted, will break out (tag-op_id is `head`)
    P5R_VS   = visible — about to process slot (op_id is real)
    P5R_AK   = wrote resp_op_id + flushed (op_id is real)

Usage:
  scripts/iter12A_p5_postproc.py <probe_dir>
    <probe_dir> contains g3/probe.* and g4/probe.* files
"""

import os
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

HEADER_BYTES = 16
FRAME_BYTES = 24
HEADER_FMT = "<IIQ"   # magic, capacity, count
FRAME_FMT = "<8sQQ"   # tag(8), ts_packed(8), op_id(8)


def parse_probe_file(path):
    """Yield (tag, tsc_cycles, cpu_id, op_id) tuples."""
    try:
        with open(path, "rb") as f:
            header_buf = f.read(HEADER_BYTES)
            if len(header_buf) < HEADER_BYTES:
                return
            magic, capacity, count = struct.unpack(HEADER_FMT, header_buf)
            if magic != 0x424F5250:  # "PROB"
                return
            if count > capacity:
                count = capacity
            for _ in range(count):
                frame = f.read(FRAME_BYTES)
                if len(frame) < FRAME_BYTES:
                    break
                tag_raw, ts_packed, op_id = struct.unpack(FRAME_FMT, frame)
                tag = tag_raw.rstrip(b"\x00").decode("ascii", errors="ignore")
                tsc_cycles = ts_packed & 0x0000FFFFFFFFFFFF
                cpu_id = (ts_packed >> 48) & 0xFFFF
                yield (tag, tsc_cycles, cpu_id, op_id)
    except Exception as ex:
        print(f"WARN: failed to parse {path}: {ex}", file=sys.stderr)


def decode_op_id_host(op_id):
    """host_id = (op_id high byte) - 1"""
    h = (op_id >> 56) & 0xFF
    return int(h - 1) if h > 0 else -1


def main():
    probe_dir = Path(sys.argv[1])
    if not probe_dir.is_dir():
        print(f"not a directory: {probe_dir}", file=sys.stderr)
        sys.exit(1)

    # Collect events grouped by op_id across both hosts
    # events[op_id] = [(host, pid, tid, tag, tsc, cpu), ...]
    events_by_op = defaultdict(list)
    # gap events keyed by `head` (P5R_GZ / P5R_GX) since they don't carry op_id
    gap_events_g3 = []
    gap_events_g4 = []

    file_count = 0
    frame_count = 0
    for host in ("g3", "g4"):
        host_dir = probe_dir / host
        if not host_dir.is_dir():
            continue
        for path in sorted(host_dir.glob("probe.*")):
            m = re.match(r"probe\.(\d+)\.(\d+)", path.name)
            if not m:
                continue
            pid, tid = int(m.group(1)), int(m.group(2))
            file_count += 1
            for tag, tsc, cpu, op_id in parse_probe_file(path):
                frame_count += 1
                if tag in ("P5R_GZ", "P5R_GX"):
                    if host == "g3":
                        gap_events_g3.append((pid, tid, tag, tsc, cpu, op_id))
                    else:
                        gap_events_g4.append((pid, tid, tag, tsc, cpu, op_id))
                else:
                    events_by_op[op_id].append((host, pid, tid, tag, tsc, cpu))

    print(f"# parsed {file_count} probe files, {frame_count} total frames", file=sys.stderr)
    print(f"# unique op_ids: {len(events_by_op)}", file=sys.stderr)
    print(f"# gap events: g3={len(gap_events_g3)}, g4={len(gap_events_g4)}", file=sys.stderr)

    # Classify op_ids: by worker exit (P5W_OK vs P5W_TO)
    ok_ops = []
    to_ops = []
    no_sx_ops = []  # no spin_wait exit recorded (process killed mid-spin?)
    for op_id, evs in events_by_op.items():
        tags = {e[3] for e in evs}
        if "P5W_OK" in tags:
            ok_ops.append(op_id)
        elif "P5W_TO" in tags:
            to_ops.append(op_id)
        else:
            no_sx_ops.append(op_id)

    print(f"# worker exits: OK={len(ok_ops)}, TIMEOUT={len(to_ops)}, no-sx={len(no_sx_ops)}", file=sys.stderr)

    if not to_ops:
        print("# NO TIMEOUT events — this rep was a WIN. Nothing more to analyze.")
        sys.exit(0)

    # Detailed per-op timeline for TIMEOUT events
    # Pick first 5 + last 5 timeout ops to show
    to_sorted = sorted(to_ops)
    sample_ops = to_sorted[:5] + (to_sorted[-5:] if len(to_sorted) > 10 else [])

    print()
    print("===== TIMEOUT op_id timelines (sample) =====")
    for op_id in sample_ops:
        evs = sorted(events_by_op[op_id], key=lambda e: (e[0], e[4]))  # by host then ts
        host_origin = decode_op_id_host(op_id)
        # Find ts ref (P5W_FA) per host to normalize
        ts_ref = {}
        for h, pid, tid, tag, tsc, cpu in evs:
            if tag == "P5W_FA":
                ts_ref[h] = tsc
        if "g3" not in ts_ref and "g4" not in ts_ref:
            continue
        print(f"\nop_id=0x{op_id:016x} (origin host={host_origin})")
        for h, pid, tid, tag, tsc, cpu in evs:
            ref = ts_ref.get(h, tsc)
            d_cycles = tsc - ref
            d_ns = d_cycles / 2.0  # assume 2 GHz CPU; rough
            print(f"  [{h} pid={pid} tid={tid} cpu={cpu}] {tag:10s} dt=+{d_ns:>10.0f}ns ({d_cycles:>10d} cyc)")

    # Aggregate statistics for ALL timeout ops
    print()
    print("===== Aggregate statistics — TIMEOUT ops =====")
    print(f"Total timeout op_ids: {len(to_ops)}")

    deltas_keys = [
        ("FA→SR", "P5W_FA", "P5W_SR"),       # fetch_add cost
        ("SR→SF", "P5W_SR", "P5W_SF"),       # slot-free wait
        ("SF→PP", "P5W_SF", "P5W_PP"),       # memcpy + flush + fill control
        ("PP→PT", "P5W_PP", "P5W_PT"),       # publish itself (store + flush + fence)
        ("PT→SX_TO", "P5W_PT", "P5W_TO"), # ack-wait (should be ~5ms)
    ]
    for label, a, b in deltas_keys:
        deltas = []
        for op_id in to_ops:
            ts_a = ts_b = None
            for h, pid, tid, tag, tsc, cpu in events_by_op[op_id]:
                if tag == a: ts_a = tsc
                if tag == b: ts_b = tsc
            if ts_a is not None and ts_b is not None:
                deltas.append(ts_b - ts_a)
        if deltas:
            deltas.sort()
            n = len(deltas)
            p50 = deltas[n // 2] / 2.0   # ns @ 2 GHz
            p99 = deltas[min(n - 1, int(n * 0.99))] / 2.0
            mn = min(deltas) / 2.0
            mx = max(deltas) / 2.0
            print(f"  {label}:  n={n}  min={mn:.0f}ns  p50={p50:.0f}ns  p99={p99:.0f}ns  max={mx:.0f}ns")
        else:
            print(f"  {label}:  n=0 (no events)")

    # Did receiver SEE each timeout op? cross-host detection
    receiver_saw_to = 0
    receiver_ack_to = 0
    for op_id in to_ops:
        tags = {e[3] for e in events_by_op[op_id]}
        if "P5R_VS" in tags:
            receiver_saw_to += 1
        if "P5R_AK" in tags:
            receiver_ack_to += 1
    print()
    print(f"# Timeout ops where RECEIVER saw req_op_id (P5R_VS): {receiver_saw_to} / {len(to_ops)}")
    print(f"# Timeout ops where RECEIVER acked (P5R_AK):         {receiver_ack_to} / {len(to_ops)}")
    print()
    print("Interpretation:")
    print(f"  - If P5R_VS is high (≈ {len(to_ops)}): receiver DID see the request → ack path is the issue (or worker can't read resp)")
    print(f"  - If P5R_VS is low (≈ 0): receiver NEVER saw the request → publish never propagated, OR receiver was looking elsewhere")
    print(f"  - If P5R_VS ≈ {len(to_ops)} but P5R_AK == 0: receiver saw req but its ack write didn't fire → write_handler hang")
    print(f"  - If P5R_VS ≈ {len(to_ops)} and P5R_AK ≈ {len(to_ops)}: receiver ack'd but worker's spin_wait didn't observe in 5ms → CXL receiver→worker visibility issue")

    # Same for OK ops as control
    if ok_ops:
        receiver_saw_ok = sum(1 for op_id in ok_ops if any(e[3] == "P5R_VS" for e in events_by_op[op_id]))
        receiver_ack_ok = sum(1 for op_id in ok_ops if any(e[3] == "P5R_AK" for e in events_by_op[op_id]))
        print()
        print(f"# CONTROL (OK ops): n={len(ok_ops)}, receiver-saw={receiver_saw_ok}, receiver-ack={receiver_ack_ok}")

    # Gap statistics from receiver side
    print()
    print("===== Receiver gap statistics =====")
    print(f"g3 receiver gap-encounter events (P5R_GZ): {sum(1 for e in gap_events_g3 if e[2] == 'P5R_GZ')}")
    print(f"g3 receiver gap-exhaust events (P5R_GX):   {sum(1 for e in gap_events_g3 if e[2] == 'P5R_GX')}")
    print(f"g4 receiver gap-encounter events (P5R_GZ): {sum(1 for e in gap_events_g4 if e[2] == 'P5R_GZ')}")
    print(f"g4 receiver gap-exhaust events (P5R_GX):   {sum(1 for e in gap_events_g4 if e[2] == 'P5R_GX')}")


if __name__ == "__main__":
    main()
