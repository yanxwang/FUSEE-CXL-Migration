# FUSEE-CXL migration — design goals and validation discipline

## North-star throughput target

The CXL migration is considered **successful only if both read and
write workloads sustain ≥ 20 Mops/s** on the 2-host (g3 + g4) testbed:

- **YCSB-C** (100 % read, Zipfian) ≥ **20 Mops/s** aggregate
- **YCSB-A** (50 % read + 50 % update, Zipfian) ≥ **20 Mops/s** aggregate

Until both bars are met, the migration is **incomplete**. A 3–5×
improvement over the prior baseline is not a stopping condition — it
is a checkpoint; measure the remaining gap to 20 Mops/s explicitly
before moving on.

## Why 20 Mops/s is the right target (and how to overturn it)

20 Mops/s is derived from the hardware envelope:

- CXL Type-3 device latency: ~300 ns random 64 B read, ~200 ns
  store+flush, bandwidth ~20 GB/s per link on this fabric.
- Local DRAM: ~100 ns latency, ~70 GB/s.
- LFM fast-path (uncontended): ~3 µs per critical section (currently).
- RACE bucket layout: 2 cachelines, 7 slots, hash-addressed.

For a write to cost < 50 ns of amortized CPU time (the 20 Mops/s
budget at T = 64–86 clients/host) we have a plausible slack in the
CXL latency (one round-trip ≈ 300 ns, amortized across clients on
independent buckets). The target is not soft: it presupposes we can
drive writes mostly through **local DRAM cache + 1 CXL cacheline
publish** per op, with bucket-granularity coordination not
serializing on hot keys.

**Rule for overturning this target**: any claim that 20 Mops/s is
not reachable must come with (a) latency decomposition showing the
irreducible component and (b) an argument that no simpler ordering
or coarser granularity recovers it. Fuzzy arguments ("it's the
protocol overhead") are not sufficient.

## Analysis discipline (the rule I kept breaking on 2026-04-22)

Every time a benchmark completes, BEFORE claiming a phase is "done",
compute the delta to 20 Mops/s and the latency budget consumed:

1. For every (protocol, workload, T) cell compare to 20 Mops/s.
2. If below: compute `budget_remaining_ns = 1e9 / 20e6 - observed_p50_ns`.
   That number is negative if the op is slower than the target per-op
   budget. Pick the worst cells and add to the open investigation list.
3. For each open investigation cell, do a latency decomposition: wrap
   the hot path with rdtscp-style timers (or clock_gettime if
   simpler) around the 4–5 sub-stages:
   `lock_acquire | pre_scan | publish_slot | dispatch | bump_epoch + unlock`.
   Report the median and p99 of each stage.
4. Identify the dominant stage. Decide whether it's (a) CXL-bound
   (hardware), (b) fence/serialization overhead (tunable), (c) lock
   contention (granularity or algorithm). Only (a) is acceptable as
   "irreducible"; (b) and (c) are open work items.
5. Do NOT stop at "we got a 3x improvement". Stop when either the
   cell hits 20 Mops/s or step 4 concludes "(a) irreducible".

This discipline applies every time a result is reported — even on
non-scaling-sweep micro-benchmarks.

## Current distance to target (2026-04-22 v4 sweep)

| workload | opt | best thpt (Mops/s) | at T | gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | A | 0.60 | 4 | 33× |
| workloada | B | 0.92 | 4 | 21× |
| workloada | C | 1.08 | 4 | 19× |
| workloada | (all) @ T=32 | 0.16–0.56 | 32 | 36–125× |
| workloadc | C | 51.2 | 86 | **met** |
| workloadd | C | 45.8 | 86 | **met** |

**YCSB-C is met. YCSB-A is ~20× away** on every protocol. This is
the open work.
