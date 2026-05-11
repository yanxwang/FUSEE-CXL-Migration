# iter-11A backlog memo (carried from iter-10A)

**Date**: 2026-05-10
**Source iters**: iter-10A (this iter), iter-9A redo, iter-7A
**Format**: each item carries (a) measurement evidence from current/prior iter, (b) proposed fix, (c) predicted gain.

---

## Iter-11A first task — recommendation

**#1 forwarder-pool-direct + cross-host pool generation** is the highest-leverage single
fix per iter-9A redo backlog #3. iter-10A Phase 4 confirms R3 (forward_read) is in top-3
of every healthy cell with mean 9.3-10.5 µs. iter-9A redo predicted -37% recovery on
read-path; iter-10A's TLS cache covered ~7% of that. Remaining 30% is on this fix.

---

## Genuine deferrals from iter-10A

### #4 True fetch_add(N) sender batching

**Source**: iter-10A Phase 3.E
**Evidence**: WINNER_ANALYSIS.md — single-sender ceiling = ~700k ops/s/ring (Phase 0
baseline 1.4 µs CXL fetch_add). Multi-worker (B0) parallelizes to 10.4 Mops/s. True
fetch_add(N) batching would let single-sender amortize CXL atomic across N=16 ops →
~10× per-sender, reaching B0-class throughput on the aggregator path.

**Why deferred**: implementation hit ring-state corruption on partial-batch timeout.
Sender's req_op_id=0 clearing on a partially-filled batch causes receiver's head to
get stuck (the empty slot in the middle of a batch makes the receiver break, leaving
post-empty filled slots forever unprocessed).

**Proposed fix**: receiver-side gap-tolerance — process slots non-sequentially within
a batch by reading req_op_id of slots [head, head+K) and processing whichever are
non-zero. Requires receiver state machine rewrite (~300 LOC).

**Predicted gain**: K=16 batched + gap-tolerant receiver → 5-10 Mops/s per ring × 3
rings = 15-30 Mops/s sender-path ceiling. Would let aggregator path beat B0 at high T.

### #8 NEW: parallel inval-broadcast receiver

**Source**: iter-10A Phase 4 path_decomp on workload-a worst + workload-f best
**Evidence**: I6 stage = 591-738 µs/op mean (16-18 samples = tail-of-distribution).
1 InvalReceiver thread per ring is the bottleneck; under W-heavy workload (a, f), invals
queue up and ack latency grows linearly with depth.

**Proposed fix**: shard inval-receiver work across N (~8) DRAM-only worker threads.
InvalReceiver becomes a lightweight dispatcher; per-shard threads run cache_pool_set_stale
in parallel.

**Predicted gain**: 591 µs → ~50 µs (12× cut on tail). Should remove the worst-case
write-completion stall, opening room for the W-write path to scale to its theoretical
ceiling.

### #9 NEW: bimodal-cell instability investigation

**Source**: iter-10A Phase 5.B anomaly verification
**Evidence**: 8/210 sweep cells exhibit bimodal pattern — sometimes 1.5-13 Mops/s,
sometimes <0.05 Mops/s, with no progression between reps. 5-rep medians span 4 orders
of magnitude on the same cell.

Pattern (verified-cell list):
- workloada T=4 on kv=512: 1.55 max, 0.006 median
- workloada T=64 on kv=1024: 13.56 max, 0.12 median
- workloada T=4/8 off kv=512: 1.7-3.4 max, 0.006-0.012 median
- workloadb T=64 on kv=512: 11.86 max, 0.05 median
- workloadd T=4 off kv=512: 1.78 max, 0.004 median
- workloadf T=8 on kv=512, T=32 off kv=1024: 3.6/6.8 max, 0.017/0.048 median

**Hypothesis**: cookie-collision or cold-start race; specific cells trigger a startup
deadlock or first-op resource-exhaustion that doesn't recover within the 50k-op
measurement window.

**Investigation steps for iter-11A**:
1. Add per-rep first-op latency probe (time to op-1 completion vs total wall)
2. Run perf-record on a known-bimodal cell, capture both healthy and collapsed reps
3. Check FUSEE_RUN_COOKIE collision frequency
4. Check whether ring-tail or directory-state from previous rep interferes with next rep

---

## Genuine deferrals carried from iter-9A redo (still open)

### #3 forwarder-pool-direct + cross-host pool generation

**Source**: iter-9A redo backlog
**Evidence**: iter-9A redo Phase 4 — read-path -37% regression after 3-ring split;
iter-10A Phase 4 confirms R3 = 9.3-10.5 µs across all workloads (top-3 universal).

**Proposed fix**: forwarder thread pre-fetches read targets from cache_pool DRAM and
deposits result directly to requester's read-staging area; eliminates one CXL roundtrip
per read.

**Predicted gain**: -37% R3 → ~6 µs mean. On workload-c (100% read), expect headline
to climb from 11.7 → ~16 Mops/s.

### #5 lock-free CAS cache_pool refinement (RCU + epoch reclamation)

**Source**: iter-10A Phase 2 (seqlock CAS shipped) + Phase 4 finding
**Evidence**: Phase 4 shows W10 mean 4.2-6.6 µs (seqlock CAS) but p99 19-25 µs — CAS
retry storms under high contention. Spinlock had p99 5.2 µs.

**Proposed fix**: replace seqlock with RCU + epoch-based reclamation. Readers never
block, never retry. Writers cooperate via epoch advancement.

**Predicted gain**: W10 p99 25 µs → 8-10 µs, freeing ~5 µs from write tail.

### #6 hot-bucket sharding within owner

**Source**: iter-9A redo + iter-5C long-standing
**Evidence**: workload-A Zipf head-key contention; visible in W10 p99 tail and R1
spread.

**Proposed fix**: detect hot bucket (lookup count > 10× mean), shard into N=8
sub-buckets keyed by hash(key) >> N_low_bits.

**Predicted gain**: workload-A T=64 cache=on kv=1024 from 9.3 → 14-16 Mops/s.

### #7 variable-length keys

**Source**: iter-9A redo backlog
**Status**: still open; not blocking 20-Mops/s gate.

---

## Carried from iter-7A

### BucketLockTable removal (-2.5 GB CXL)

**Status**: still open; iter-10A doesn't touch CXL layout.

---

## Living docs full §II.3-II.6 narrative rewrite

**Source**: iter-9A redo carried as amendment
**Status**: still open; partial updates landed in iter-10A (II.1, II.2 for TLS cache)
but the full P/W/R narrative rewrite remains a separate doc-only iter.
