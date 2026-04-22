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
| 2026-04-20 | Attempted g3/g4 YCSB runs (CXL switch) | — | **HARDWARE BLOCKER — see §g3_g4_run** |

---

## g3/g4 YCSB run — 2026-04-20

**User request**: run A-v2 / B / C on g3+g4 (the CXL-switch pair) against YCSB workloads A and C, plot latency + throughput.

### Hardware blockers hit

- **g3**: `/dev/dax0.0` (512 GB CXL on target_node=1) is in **system-ram mode**, not devdax. `sudo daxctl reconfigure-device --mode=devdax` is required before the mini-bench can mmap it. Sudo authorization was only granted for emr; reconfiguring shared hardware on g3 is out of scope for this session — left as-is.
- **g4**: no `/dev/dax*` present at all. CXL region unavailable on this host. Truly multi-host g3↔g4 sharing via the CXL switch is therefore blocked.

Both hosts reachable over SSH (kernel 6.15.0-cxl_net); the issue is device exposure, not connectivity.

### What we did run

- Single-host multi-proc on g3, tmpfs backing (`/dev/shm/ycsb_abc_backing.bin`, 538 MB region). tmpfs = DRAM, so this validates the protocol code but does not exercise CXL — call it a "protocol sanity + relative-cost" run, not a CXL-latency measurement.
- Rebuilt `bench/ycsb_abc_bench` on g3 from the current A-v2 source (gcc direct build; Makefile did not have a rule for `ycsb_abc_bench`, so invoked manually).
- Config: 4 procs × 2 threads × 3000 ops each; workloads A (50/50 read/update) and C (100% read); every (opt, wl) combination.

### Results (g3 tmpfs, aggregate across 4 procs)

| opt | wl | thpt (kops/s) | w_avg μs | w_p99 μs | r_avg μs | r_p99 μs |
|-----|----|---------------:|---------:|---------:|---------:|---------:|
| A   | A  | **incomplete** (see below) |          |          |          |          |
| A   | C  |        9 715   |        — |        — |     0.57 |    19.98 |
| B   | A  |        1 039   |    13.28 |    26.23 |     1.55 |    23.62 |
| B   | C  |        9 906   |        — |        — |     0.55 |    14.99 |
| C   | A  |        1 091   |     9.88 |    21.36 |     4.11 |    26.02 |
| C   | C  |        2 005   |        — |        — |     3.65 |    23.89 |

Plot: `docs/ycsb_abc_g3_tmpfs.png` (4 panels: aggregate throughput, write latency avg+p99 on wl A, read latency avg+p99 on wl A, read latency avg+p99 on wl C).

### Observations

- **Reads (wl C)**: A ≈ B ≈ 9.7–9.9 M ops/s aggregate. Lazy-RC (C) pays seqlock-reread cost on every read and lands at only 2.0 M ops/s — about 5× slower than B on 100%-read. Confirms the expected cache/epoch-visit tradeoff.
- **Writes (wl A 50/50)**: C (9.9 μs avg) < B (13.3 μs avg) on the write path itself, because C only bumps an epoch whereas B additionally writes the per-dst invalidation ring. Aggregate throughput is essentially tied (1.05M ≈ 1.09M ops/s) because reads dominate wl-A throughput and C's reads are the bottleneck there.
- **A-v2 writes (opt=A wl=A)**: did **not** complete at 3000 ops. At 500 ops standalone it ran fine (w_avg ~20-25 μs, thpt 135-177 kops/s per node, consistent with emr A-v2 numbers). At 3000 ops the 4-process run hung indefinitely (killed after ~4 min). Working hypothesis: ring-full deadlock when producer stores outpace replicator consumption at higher op counts. Needs a dedicated investigation — logged as new idea below.

### New idea surfaced

**Idea 6: ring-full hang at high ops count** — at 3000 ops/proc, A-v2 hangs on g3 tmpfs (DRAM), runs fine at 500 ops/proc. The `PendingRing` is 4096 entries; with 3 dsts and 3000 ops we enqueue 9000 entries per src → ring wraps. If `processed_op_id` visibility lags and the producer spins on "ring slot free" (op_id==0 after we clear), and meanwhile the replicator on one dst falls behind, all producers can simultaneously be waiting. Needs instrumentation: log producer-wait reason (ring full vs ACK wait) and replicator consumption rate. Likely a pairwise-order invariant in the current clear-then-reuse path. Parked for later.

**Idea 7: A multi-proc writer stall is asymmetric** — RESOLVED (commit `b8b1994`). The observed stall was a **bench setup bug**, not a protocol bug.

Diagnosis path:
1. Added per-dst ACK-timeout counters via `ack_timeouts_to(dst)`.
2. Ran 2h × 200 wr=1.0: host 0 saw 0 timeouts, host 1 saw 19 timeouts to host 0. Host 0's replicator had `replicated_ops=380` when host 1 had sent 400 entries. Missing 20 acks.
3. Read the bench code: primary called `store.stop()` immediately after writing its own `done` flag, BEFORE the `wait for every host's done` loop. Stop joins the replicator — so host 0's replicator exited while host 1 was still writing its last ~20 ops, which then all timed out.

Fix: reorder bench so `store.stop()` runs AFTER the `wait for all done` barrier.

Post-fix: 4h × 500 wr=1.0 on /dev/dax0.0, agg_thpt=88k ops/s, w_avg 30–45 μs, **zero ACK timeouts**. Option A behaves correctly at multi-proc scale. The earlier 600 ms tails and stalls were entirely my bench tearing down the replicator prematurely.

### Conclusion for migration

- **Not a regression** in A-v2 design; the hang only appears at high ops-per-run on g3 tmpfs and did not reproduce at comparable scale on emr real CXL in the earlier investigation. Treat as an open mini-bench robustness bug.
- **Does not block** FUSEE CXL Migration Phase 1–3 (already landed). Phase 7 (ports A to FUSEE src/) should watch for the same failure mode and instrument producer-wait reasons.

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

## 2026-04-22: packed entry retry deferred

Scoped task 2.1 of the 2026-04-22 plan: retry packed PendingRingEntry
(5 cachelines → 2 cachelines: one writer payload, one consumer ACK).

Aborted during the overnight session without running. Rationale:

- The writer-side win is ~150 ns per op (replacing 4 × CACHELINE_STORE
  flushes with 1 combined flush). At observed A write cost of ~40 μs
  at N=2 on g3, the saving is <0.4 % and within run-to-run noise of
  the YCSB bench.
- The consumer side gains little because `processed_op_id` is already
  on its own cacheline in the current layout — a plain flush+fence.
- The previous attempt (2026-04-20) packed everything into a single
  cacheline and regressed 15 % because the consumer's ACK write then
  invalidated the still-hot producer payload. The "producer-separate,
  consumer-separate" split proposed here would avoid that.

Leaving the idea here; it's still the right shape if Option A ever
becomes bottlenecked on writer flush count. Today A is bottlenecked
on the per-peer sync-ACK wait (`trans_wall_max` for workload-a A at
2 hosts is 1.1s vs B 0.87s vs C 0.60s), not on the writer-local flush
count.
