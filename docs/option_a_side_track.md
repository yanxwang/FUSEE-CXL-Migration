# Option A Side Track — Long-running research log

> **Purpose**: Option A (sync replication with ACK) is expected to be the **slowest** of the three protocols due to fundamental sync semantics. This doc tracks **algorithmic improvements to Option A** over time. Each entry is a checkpoint: what was changed, measurable impact, and current gap vs Option B/C.
>
> **Principle**: the implementation choices here directly affect the final FUSEE CXL Migration — whatever design becomes "A" in the main bench must also be the design used when integrating A into FUSEE src/.
>
> **Current iteration**: **A-v2 (SPSC ring)**, see §2 below.

---

## Baseline (original naive design, before any investigation)

**Design**:
- `BucketLockEntry.pending_op` is set by writer to `op_id`
- Each replicator iterates `for b=0..NUM_BUCKETS` checking `pending_op != 0`
- When replicator sees a new op_id, it writes `bucket.ack[my_node] = op_id`
- Writer spins watching `ack[i] == op_id for all i != self`

**Perf on real CXL (4 proc × 1 thread × pure writes, emr)**:
- Write latency avg: **4000 μs** (writer waits for replicator's O(N) scan to reach its bucket)
- Per-node throughput: **2.5k ops/s**
- Replicator scan period: **3500-7300 μs**

**Why slow**: writer pays for replicator's full scan latency; latency is O(NUM_BUCKETS). With 4096 buckets and ~1 μs per CACHELINE_LOAD on CXL, one scan = ~3 ms.

**Verified by NUM_BUCKETS sweep** (2026-04-20 Exp 3): latency scales linearly with NUM_BUCKETS (256 → 185 μs; 16384 → 14500 μs).

---

## A-v2: SPSC ring-based notification (2026-04-20)

**Change**: replace `bucket.pending_op` flag + O(N) scan with per-(src,dst) SPSC ring.

### Data structure (what replaced what)

| Removed | Added |
|---|---|
| `BucketLockEntry.pending_op`, `pending_slot`, `pending_new_value`, `ack[MAX_HOST_NUM]` (on every bucket) | Per-(src,dst) `PendingRing` of 4096 entries |

Ring entry (fixed size, ~320B per entry with cacheline padding):
```c
typedef struct {
    cacheline_u64 op_id;              // publish-last flag
    cacheline_u64 bucket_idx;
    cacheline_u64 slot_idx;
    cacheline_u64 new_value;
    cacheline_u64 processed_op_id;    // replicator sets = op_id when done
} PendingRingEntry;

typedef struct {
    cacheline_u64 tail;   // producer cursor (src)
    cacheline_u64 head;   // consumer cursor (dst)
    PendingRingEntry entries[4096];
} PendingRing;

PendingRing pending[MAX_HOST_NUM][MAX_HOST_NUM];  // rings[src][dst]
```

Memory cost: MAX_HOST_NUM² × ring size = 16 × 4096 × 320B ≈ 20 MB per region (vs negligible bucket flags in baseline, but buckets now only have `write_epoch` + `staging_scratch`).

### Writer protocol (A-v2)

1. lock bucket (LFM)
2. scan bucket for empty slot
3. allocate KV + stage to CXL
4. **for each dst != my:** enqueue to `pending[my][dst]`:
   - claim slot at `tail++` (atomic_fetch_add on local cursor; single-producer)
   - wait if ring slot still occupied (`op_id != 0`)
   - fill `bucket_idx`, `slot_idx`, `new_value`, `processed_op_id=0`
   - publish: set `op_id = op_id` last (release-semantic via CACHELINE_STORE)
   - advance `tail`
5. **spin-wait:** poll each dst's `entries[].processed_op_id == op_id`
6. **clear slots:** set `op_id = 0` on all dst's entries so replicator can reuse
7. commit slot to authoritative bucket
8. unlock

### Replicator protocol (A-v2)

Per node X, replicator polls only N-1 ring heads:
```
loop:
  for src in 0..N, src != X:
    while head < tail of pending[src][X]:
      entry = pending[src][X].entries[head % RING_SIZE]
      if entry.op_id == 0: break  # not yet published
      ... do simulated pull (staging fetch) ...
      entry.processed_op_id = entry.op_id
      head++
```

### Perf on real CXL (same setup as baseline)

| Metric | Baseline A | **A-v2** | Improvement |
|---|---|---|---|
| Write latency avg | 4000 μs | **18-25 μs** | **~180x** |
| Per-node throughput | 2.5k ops/s | **40-55k ops/s** | **~20x** |
| Replicator scan period | 3500-7300 μs | **1.2-1.6 μs** | **~3000x** |

### Remaining gap vs B/C (on same setup)

| Option | Write latency | Thpt/node |
|---|---|---|
| A-v2 | 18-25 μs | 40-55k |
| B | 10-16 μs | 62-95k |
| C | 7-11 μs | 81-133k |

A-v2 is **~2x slower than B, ~2.5x slower than C**. This is the **inherent sync-replication cost**:
- Writer must wait for ACK from every other node
- Even with O(1) ring polling, wait time = max(replicator latencies) ≈ 5-15 μs
- Plus extra stores for enqueue: 3 dst × 5 CACHELINE_STOREs = 15 stores ≈ 12 μs on CXL

### Fundamental minimum (analytical)

```
A-v2 writer lower bound = 
  lock (5 ops)                     +~4μs
  bucket scan (1 load)             +~1μs
  staging (4 stores for KV)        +~3μs  (simulated)
  enqueue to N-1 rings             +~3×(5 stores + 1 tail store) ≈ 14μs
  WAIT for all (N-1) ACKs          +~10-15μs (unavoidable)
  commit (1 store)                 +~1μs
  unlock (2 stores)                +~2μs
  ──────────────────────────────────
  Total                            ~35-40μs absolute floor

B writer = everything except waits ≈ ~12-15μs (just lock, stage, commit, push, unlock)
```

So A-v2 fundamentally has ~20μs ceiling due to sync wait. We measured ~20μs, matching analytical minimum.

### Integration into main bench

**File**: `cxl_shm_profiling/bench/ycsb_abc_bench.c`

- Removed fields from `BucketLockEntry`: `pending_op`, `pending_slot`, `pending_new_value`, `ack[]`
- Added `staging_scratch` cacheline field on `BucketLockEntry` to simulate staging/OpLog flush cost for B and C (which previously reused `pending_new_value`)
- Added `PendingRingEntry` + `PendingRing` types, embedded in `SharedRegion.pending[src][dst]`
- Rewrote `kv_insert_A` from scan-based to ring-based
- Rewrote `replicator_thread` A branch from `for b in NUM_BUCKETS` to `for src in MAX_HOST_NUM` with per-ring head cursor

---

## Ideas not yet tried (future iterations)

### Idea 1: Pack ring entry fields into single cache line

**Attempted 2026-04-20 but reverted** because naive packed-layout version (plain writes + single flush_line + store_fence) was **worse** than cacheline_u64 version. Root cause unclear. Hypotheses:
- Reader does `flush_line + full_fence` before each field read — might pay multiple full-line flushes
- CACHELINE_STORE's explicit sequence may be better-optimized than manual equivalent
- Compiler reordering despite `__asm__ __volatile__("": : :"memory")`

**Worth revisiting** with more careful writer/reader code — might save ~3-5 μs per write.

### Idea 2: Write pipelining / batch ACKs

If writer has multiple in-flight ops, can it submit all of them then collect ACKs at end? Would amortize the wait-for-ACK over multiple ops.

- Complicates the programming model (coroutine-friendly but synchronous-API-unfriendly)
- Could match B performance for bursty workloads

### Idea 3: Reduce ACK granularity

Instead of per-op ACK, use per-batch ACK. Writer submits batch of K ops in one ring entry; replicator ACKs once the whole batch is processed.

- Amortizes the "wait for first ACK" latency
- Restricts to independent ops (no within-batch dependency)

### Idea 4: Co-locate pending ring with bucket locks by affinity

If workload has hot buckets, the writer and replicator may hit the same cache line. By placing rings adjacent to the buckets they serve, we might get better cache behavior.

- Complex memory layout, needs NUMA-aware allocation
- Benefit unclear until measured

### Idea 5: Quorum ACK (N/2+1 instead of N-1)

If writer waits for majority of ACKs instead of all, latency = 2nd-fastest replicator instead of slowest. Matches Paxos semantics. Tradeoff: weaker durability guarantee (failure of slowest node + one other could lose data).

- Almost always a 20-40% latency improvement on average
- Common in production systems (Raft, Multi-Paxos)
- Worth investigating for a paper-friendly variant

---

## Log of changes

| Date | Change | Files touched | Perf delta |
|---|---|---|---|
| 2026-04-20 | Baseline (original scan-based design) | `ycsb_abc_bench.c` | — |
| 2026-04-20 | Investigation: confirm O(NUM_BUCKETS) root cause | `ycsb_abc_bench_inst.c` (new), sweep scripts | diagnostic only |
| 2026-04-20 | A-v2: SPSC ring implementation | `ycsb_abc_bench_av2.c` (new) | +180x write latency |
| 2026-04-20 | Integrate A-v2 into main bench; remove pending_op fields | `ycsb_abc_bench.c` | (same as standalone A-v2) |
| 2026-04-20 | Tried packed ring entry, reverted (slower) | (reverted) | -2x (discarded) |

---

## How this side-track affects the main FUSEE CXL Migration

When implementing the FUSEE `src/cxl_kv_ops.cc` (Phase 3/7 of main migration):

1. **Option A**: must use the SPSC ring design (A-v2), not the naive scan. This means:
   - New data structures: `PendingRingEntry`, `PendingRing`, in some CXL-region layout
   - Writer path: enqueue + spin-wait on processed_op_id
   - Replicator fiber (new code): poll N-1 rings

2. **Common infrastructure** needed by A but not by B or C:
   - `cxl_region_layout.h` must include `PendingRing` array (between bucket locks and staging)
   - Crash recovery: OpLog must record ring position to allow replay of unfinished ops

3. **Phase ordering**: since A is most complex, we should do A **last** in Phase 7. Start with C (simplest), then B, then A.

4. **If we discover more A optimizations during migration** (e.g., packed entries finally works), we retroactively update both this doc and the main bench.
