# Option A Performance Investigation

**Date**: 2026-04-20 02:00-02:30 CDT
**Host**: emr (Intel Xeon Gold 6530, 128 GB DRAM + 256 GB CXL Type 3)
**Scope**: explain why Option A write throughput is ~100-400x lower than B/C in mini-bench, and fix if implementation artifact

## TL;DR

- **Root cause**: original Option A's replicator polls `pending_op` of **every bucket** (4096 in mini-bench). Writer waits for replicator to scan around to its specific bucket. Latency ∝ NUM_BUCKETS.
- **Not fundamental** to sync-replication: fix = use per-(src,dst) SPSC ring so replicator polls ring heads (~4 rings) instead of bucket table.
- **A-v2 (ring-based)**: writer latency drops from ~4000 μs → ~20-55 μs on real CXL, **~100x faster**.
- **Remaining gap vs B/C**: A-v2 still 2-3x slower than B/C; this is the **inherent sync-replication tax** (wait for N-1 ACKs).

## Experiments

### Exp 1 + 2: Writer wait breakdown on Original A

Instrumented `ycsb_abc_bench_inst.c` with 7 timestamps per write (T0=lock, T1=staging done, T2=pending set, T3=first ACK, T4=last ACK, T5=commit, T6=unlock).

**Setup**: 4 processes × 1 thread × 300 pure writes.

**tmpfs results**:

| Phase | Node 0 | Node 1 | Node 2 | Node 3 |
|---|---|---|---|---|
| lock + scan + staging (T1-T0) | 7.0 μs | 7.1 μs | 7.0 μs | 9.9 μs |
| set pending + sfence (T2-T1) | 0.03 μs | 0.04 μs | 0.03 μs | 0.03 μs |
| **wait first ACK (T3-T2)** | **2549 μs** | **1415 μs** | **1386 μs** | **1382 μs** |
| wait last ACK (T4-T3) | 26 μs | 1559 μs | 1529 μs | 1586 μs |
| commit + clear (T5-T4) | 0.03 μs | 0.07 μs | 0.10 μs | 0.19 μs |
| unlock (T6-T5) | 0.03 μs | 0.03 μs | 0.03 μs | 0.03 μs |
| **TOTAL** | **2582 μs** | **2982 μs** | **2923 μs** | **2978 μs** |

**CXL results**:

| Phase | Node 0 | Node 1 |
|---|---|---|
| lock+scan+staging | 11.8 μs | 9.3 μs |
| **wait first ACK** | **1572 μs** | **1668 μs** |
| wait last ACK | 2543 μs | 2170 μs |
| TOTAL | **4127 μs** | **3847 μs** |

**Observation**: writer spends **97-99% of total time** waiting for ACKs. Lock/staging is only 7-12 μs; commit+unlock is negligible (~100 ns).

### Replicator scan period (also Exp 1)

Replicator in Original A loops over all 4096 buckets checking `pending_op`:

```c
for (int b = 0; b < NUM_BUCKETS; b++) {
    uint64_t p_id = CACHELINE_LOAD(&e->pending_op);   // most are 0
    if (p_id != 0 && ...) { /* do work, ACK */ }
}
```

Measured scan-period (one full loop):

| Backing | avg | p50 | p99 |
|---|---|---|---|
| tmpfs | 3340 μs | 3425 μs | 3960 μs |
| tmpfs (more contention) | 5224 μs | 5243 μs | 5418 μs |
| CXL | 3500-7300 μs | 3450-7440 μs | 4880-7720 μs |

Writer's "wait first ACK" = half the scan period (random bucket distribution). This **exactly matches** the 1400-2500 μs we measured.

→ **Hypothesis confirmed: Option A's latency is O(NUM_BUCKETS) scan delay.**

### Exp 3: NUM_BUCKETS Sweep

Recompiled Original A with NUM_BUCKETS ∈ {256, 1024, 4096, 16384}.

**Total writer latency** (avg across 4 nodes, μs):

| NUM_BUCKETS | tmpfs | CXL |
|---|---|---|
| 256 | 130 | 185 |
| 1024 | 400 | 440 |
| 4096 | 3160 | 3870 |
| 16384 | 8475 | 14530 |

**Linearity** (roughly):
- tmpfs: 16384/256 = 64x buckets → 65x latency ✓
- CXL: 16384/256 = 64x buckets → 79x latency (close to linear, some noise)

Proves **A's latency scales linearly with NUM_BUCKETS** → the bottleneck is definitely the scan loop.

### Exp 4: A-v2 — Ring-Based Notification

**Design**: replace `bucket.pending_op` with per-(src,dst) SPSC ring:

```c
typedef struct {
    cacheline_u64 op_id;          // 0 = empty; non-zero = active (published last)
    cacheline_u64 bucket_idx;
    cacheline_u64 slot_idx;
    cacheline_u64 new_value;
    cacheline_u64 processed_op_id; // replicator sets = op_id when done
} RingEntry;

typedef struct {
    cacheline_u64 tail;  // producer
    cacheline_u64 head;  // consumer
    RingEntry     entries[4096];
} PendingRing;

PendingRing rings[MAX_HOST_NUM][MAX_HOST_NUM];  // rings[src][dst]
```

**Writer protocol**:
1. Lock bucket (LFM)
2. Find empty slot
3. For each dst node: fill `rings[my][dst].entries[tail]` with op details; publish by storing op_id last
4. Wait until all dst rings' `processed_op_id == op_id`
5. Mark entries free; commit slot; unlock

**Replicator protocol** (on node X):
1. Loop `src ∈ {0..N-1} \ X`: read `rings[src][X]` head/tail
2. If head < tail: read entry, simulate pull, write ACK (processed_op_id), advance head
3. Only **N-1 rings** to poll, not NUM_BUCKETS

**Results on tmpfs (4 nodes × 300 writes)**:

| Metric | Original A | A-v2 |
|---|---|---|
| Writer avg total | 2900 μs | **27-60 μs** |
| Replicator scan period | 3400 μs avg | **2-3 μs avg** |
| Throughput per node | ~2.5k | **16-33k** |

**Results on real CXL (4 nodes × 300 writes, pure write)**:

| Metric | Original A | A-v2 |
|---|---|---|
| Writer avg total | ~4000 μs | **19-55 μs** |
| Replicator scan period | ~3500-7300 μs | **1.2-1.6 μs** |
| Throughput per node | ~2.5k | **17-46k** |

### Comparison with B / C on real CXL (same setup, pure writes)

| Protocol | Writer latency avg | Thpt per node |
|---|---|---|
| Original A | ~4000 μs | ~2.5k |
| **A-v2** | **19-55 μs** | **17-46k** |
| B | 11-17 μs | 60-87k |
| C | 8-11 μs | 90-125k |

## Key findings

1. **Original A was ~100x slower than it needed to be** due to O(N) scan polling, not inherent to sync-replication.
2. **A-v2 closes most of the gap**: goes from 200-400x slower than B/C → only 2-3x slower.
3. **Remaining 2-3x gap is fundamental** to sync-replication: writer MUST wait for (N-1) ACKs. Each replicator response cycle is 1-2 μs on CXL; with scheduling variance across 3 replicators the tail is ~15-30 μs.
4. **Tradeoff recap**:
   - A-v2 gives durable sync replication at ~20-55 μs write latency
   - B: commit + fire-and-forget push, 11-17 μs, no sync durability
   - C: commit + epoch bump, 8-11 μs, readers see stale until own access

## Root cause summary

```
Original A:
  writer ── pending_op=X ──→ bucket.pending_op (1 store)
                    │
                    └─── replicator polls ALL 4096 buckets ← SCAN COST DOMINATES
                                            │
                                            └── writes ack when seen

A-v2:
  writer ── enqueue ──→ rings[src][dst] (~5 stores per dst × 3 dsts)
                    │
                    └─── replicator polls 3 ring heads ← O(1) WORK
                                            │
                                            └── writes ack immediately
```

**The fix was to change from "publisher sets flag, everyone polls flag" to "publisher enqueues to dedicated per-consumer ring".** Classic message passing vs polling tradeoff.

## Files

| File | Purpose |
|---|---|
| `bench/ycsb_abc_bench_inst.c` | Instrumented Original A with timing breakpoints |
| `bench/ycsb_abc_bench_av2.c` | A-v2 implementation with ring-based notification |
| `bench/run_option_a_sweep.sh` | NUM_BUCKETS sweep |
| `bench/option_a_sweep.log` | Raw sweep data |

## Implications for FUSEE CXL Migration

**Recommendation**: When integrating Option A into FUSEE src/, **use the A-v2 design (ring-based)**, not the naive "all bucket pending_op + scan" design. This makes A a legitimate option for workloads requiring durable sync replication, at ~2-3x cost vs B/C.

Updated decision matrix:

| Requirement | Best option |
|---|---|
| Best write throughput | **C** (Lazy RC) |
| Read-heavy mixed | **B** (Eager Push) |
| Durable sync replication required | **A-v2** (ring-based), not original A |
| Default production | **C** |

## Next steps

1. Integrate A-v2 into main `ycsb_abc_bench.c` as the `OPT_A` implementation (replace the scan-based one)
2. Rerun full A/B/C comparison with A-v2 for updated documentation
3. Resume FUSEE CXL Migration Phase 1 with A-v2 as the A protocol spec
