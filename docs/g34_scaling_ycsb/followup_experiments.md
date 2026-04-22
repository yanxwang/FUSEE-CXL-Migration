# Proposed follow-up experiments after the 240-run scaling sweep

Based on what the 2026-04-22 sweep revealed. Listed in rough priority
order (my judgment; user can re-rank).

## P1 — Bucket-count sweep for Opt C write-heavy workloads

**Observation**: C on workload a (50 % writes) peaks at T=4 @ 1.12 M and
degrades to <300 k by T=64. C on workload b (5 % writes) peaks at T=16
@ 6.3 M and degrades to ~1.9 M by T=86. Same bucket count (65 536)
for both. **Hypothesis**: the degradation is LFM bucket contention.

**Plan**: hold T=86 fixed, sweep `num_buckets ∈ {16 k, 65 k, 256 k, 1 M}`
for workloads a / f (write-heavy). If degradation flattens at higher
bucket count → bucket contention confirmed; safe to recommend a bigger
hash-table config for write-heavy tenants. If degradation persists →
the bottleneck is elsewhere (memory server bandwidth? CPU cross-socket?
dig deeper with perf).

Expected runtime: 2 × 5 bucket counts × 2 workloads × 2 cache = 20 runs
≈ 5-10 min.

## P2 — Per-client Option A/B pending-ring refactor

**Observation**: A and B were clamped to 1 worker per host. The
flat lines in the 30-plot deck are honest but uninteresting.

**Plan** (code):
- `cxl_pending_ring.h`: widen `PendingRingMatrix` from `rings[H][H]` to
  `rings[H × T_max][H × T_max]`. Each row is one *client* (global id),
  not one *host*.
- `cxl_kv_ops_A.cc` / `cxl_kv_ops_B.cc`: writer pushes into
  `rings[self_global_id][peer_global_id]` for every peer (not just peer
  host). Replicator on each client consumes only `rings[*][self]`.
- Cost: matrix size grows T² — for T=86 that's 172² = ~30 k rings × 1.5 MB
  each ≈ 45 GB. Too big. Needs a sparser representation: lazy allocation
  (allocate ring on first push) or per-pair tables keyed by (src, dst)
  pairs that are actually used.
- **Simpler alternative**: keep `rings[H][H]` but add an atomic tail on
  each ring so multiple intra-host clients can safely FIFO-push. Needs
  `fetch_add` on the tail cursor + per-slot reservation. Then one
  replicator per host (not per client) is fine.

If this works, re-run the full 240-run sweep with unclamped A/B. The
interesting question: does A/B with real scaling overtake C on
mid-write workloads, thanks to async / sync replication hiding CXL
latency?

**Effort**: ~1 day of careful code.

## P3 — Replace LFM with a bounded-retry ticket lock

**Observation**: The latency decomposition from earlier this session
already pinned LFM as the 4-27 μs variable-cost component. The scaling
sweep confirms at T ≥ 32 the LFM slow path dominates p99 (seen as
100+ μs p99 write latency in C at high T).

**Plan**: keep LFM for small-N benches but offer a per-region config
flag to swap in a ticket lock (`shm_ticket_lock.c` — to be written).
Ticket lock cost: one `fetch_add` to take a ticket + spin on `now ==
my_ticket`. O(1) worst-case. At T=86, ticket lock should give bounded
write latency instead of the current tail explosion.

**Effort**: ~200 LOC for the lock + plumbing through BucketLockEntry.
Then re-run workload-a/f at T=86.

## P4 — Explain workload-b vs workload-d divergence

**Observation**: workload-b (95% R / 5% U) peaks at T=16 @ 6.3 M, then
drops to ~1.9 M at T=86 (3× loss). Workload-d (95% R / 5% I-latest)
peaks at T=86 @ 44 M, no drop.

The two workloads differ in the 5 %:

- b: UPDATE to random key → touches random bucket → random contention
  that amplifies at high T.
- d: INSERT of a "latest" key → touches recent-history bucket → could
  in principle be MORE contended (narrow bucket range), but we see
  more scaling headroom.

**Plan**: add bucket-hit histograms to the runner so we can confirm the
contention shape per workload. Or use `perf` on one run to see where
the CPU time goes for each.

## P5 — Implement workload-e (SCAN) for completeness

**Rationale**: the user explicitly skipped scan for this sweep. Once
A/B scale, workload-e is the natural stress test for C's optimistic
read path: a scan does N sequential reads; with 172 clients each doing
100 k scans of size 10, that's 170 M logical reads → CXL fabric ceiling
benchmark.

**Plan**: RACE hash doesn't naturally support scan (hash ordering is
not semantic). Two options:
- Add a parallel sorted index (e.g., a simple range-partitioned
  skip-list) on top of the hash table → scan goes through the skip-list.
- Approximate scan: bucket-level linear probe across a range of bucket
  indices (not semantic YCSB scan but exercises the same CXL traffic
  shape).

**Effort**: 2-3 days for the sorted index approach.

## P6 — Cross-host vs single-host isolation

**Observation**: our cross-host setup always uses 2 hosts. We don't
know how much of the throughput is "both machines' memory servers in
parallel" vs "one machine doing all the work while the other coordinates".

**Plan**: rerun the single-host fork sweep (T=1,2,4,8,16,32,64,86 on
g3 alone, no g4) at same T. Compare to 2-host data. Expected: 2-host
gives ~2× at any T if both hosts contribute equally. If only 1.3×,
cross-host coordination is taxing. If >2× (unlikely), something's off.

**Effort**: 10 min (120 runs of the single-host variant of the runner).

## P7 — Investigate degradation tail on C write-heavy

At T > peak, C's workload-a throughput drops from 1.1 M → 0.2 M. Is
this a smooth "contention ramp-up" or a threshold effect?

**Plan**: bisect between T=4 (peak) and T=86 with finer grain: T =
6, 10, 12, 14, 20, 24, 28, 40, 48, 56. Helps distinguish
"exponential contention" from "threshold crash" (e.g., LFM tail
ignition at a critical load).
