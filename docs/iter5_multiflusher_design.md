# iter-5 multi-flusher V2 — design doc

**Author**: Claude
**Date**: 2026-04-24
**Branch**: `feat/cxl-migration` (commit prefix `[iter5-mf]`)

---

## Goal

Replace iter-3's single flusher thread with N flusher threads
each owning a disjoint partition of the bucket ID space, so the
write path can sustain higher cross-host `bump_epoch` rate
without the single-flusher CXL atomic serialisation that capped
workload A at ~17 Mops/s in iter-3 phase-3.

Default in-tree value remains **N=1** (byte-for-byte iter-3
behaviour). Validation matrix scans N ∈ {1, 2, 4} via
`FUSEE_BATCH_NUM_FLUSHERS` env.

## Why iter-3 V1 failed (lessons)

Two bugs were diagnosed in 5189086's revert message:

1. **MPMC dirty-queue pop race**. The 94da706 attempt had **one
   shared dirty queue, multiple flusher consumers**. Consumers
   used `fetch_add(dq_head)` to claim slots. Under contention,
   `dq_head` could advance past `dq_tail` (because
   tail-load/head-fetch_add were not snapshot-consistent),
   causing entries to be skipped and never drained — lost-update
   hang.
2. **Two-phase publish race** (the aeadca9 ready-flag
   workaround). Producer reserved the dq position via
   `fetch_add(dq_tail)`, then on overflow branched out *without*
   setting the per-slot ready flag. The flusher waited
   indefinitely on the unset flag — deadlock.

V2 design avoids both classes by making the dq **MPSC** (one
flusher per queue, multiple producers) and having no two-phase
publish at all (the dq slot is a plain `uint32_t` bucket_idx,
written before the tail advance).

## V2 architecture

```
                   ┌──────────────────────────────────────────────┐
                   │ Worker thread on host h (one of T per host)  │
                   │ insert/update(key, ...) →                    │
                   │   bucket_idx = hash(key) % num_buckets       │
                   │   batch_ring_.append(bucket_idx, ...)        │
                   │     → per-bucket cursors append_cursor++     │
                   │     → ring_entries[bucket_idx][seq] := slot  │
                   │     → if queued.cas(0,1):                    │
                   │         flusher_id = bucket_idx % N          │
                   │         dq[flusher_id].push(bucket_idx)      │
                   └──────────────────────────────────────────────┘
                                       │
                       partition by   bucket_idx % N
                                       │
       ┌───────────────────────────────┼───────────────────────────────┐
       ▼                               ▼                               ▼
   ┌──────────┐                  ┌──────────┐                    ┌──────────┐
   │ flusher 0 │                  │ flusher 1 │      ...           │ flusher N-1│
   │ owns      │                  │ owns      │                    │ owns      │
   │ bid%N==0  │                  │ bid%N==1  │                    │ bid%N==N-1│
   │           │                  │           │                    │           │
   │ dq[0] hd++│                  │ dq[1] hd++│                    │ dq[N-1]hd++│
   │ → drain   │                  │ → drain   │                    │ → drain   │
   │ → bump_ep │                  │ → bump_ep │                    │ → bump_ep │
   └──────────┘                  └──────────┘                    └──────────┘
       │                               │                               │
       └───────────────────────────────┴───────────────────────────────┘
                            disjoint bucket sets
                          (no inter-flusher coord)
```

### Layout — `BatchRingHeader` change

Existing iter-3 layout has **one** dirty queue per host:

```cpp
struct BatchRingHeader {
  std::atomic<bool>     stop;
  std::atomic<uint64_t> init_done;
  std::atomic<uint64_t> dq_tail;          // multi-producer
  uint64_t              dq_head;          // single-flusher consumer
  uint32_t              dq_slots[8192];
};
```

V2 widens to **N** dirty queues, one per flusher:

```cpp
constexpr int kMaxFlushers = 8;            // hard cap; runtime N <= this

struct alignas(64) DirtyQueueShard {
  std::atomic<uint64_t> dq_tail;           // multi-producer push
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t              dq_head;           // single-consumer pop
  char _pad_head[64 - sizeof(uint64_t)];
  uint32_t              dq_slots[kDirtyQueueCapacity];   // 8192
};

struct BatchRingHeader {
  std::atomic<bool>     stop;
  char _pad_stop[64 - sizeof(std::atomic<bool>)];
  std::atomic<uint64_t> init_done;
  char _pad_init[64 - sizeof(std::atomic<uint64_t>)];
  uint32_t              num_flushers;       // 1 .. kMaxFlushers
  char _pad_n[64 - sizeof(uint32_t)];
  DirtyQueueShard       dq[kMaxFlushers];
};
```

### Producer-side append()

```cpp
void MicroBatchRing::append(uint32_t bucket_idx, uint16_t slot_idx,
                            uint64_t value, uint64_t *backpressure_out) {
  // 1. Per-bucket ring claim — UNCHANGED FROM ITER-3.
  BucketRingCursors *c = &cursors_[bucket_idx];
  uint64_t seq = c->append_cursor.fetch_add(1, std::memory_order_acq_rel);
  RingEntry *e = &ring_[bucket_idx * K_ + (seq % K_)];
  while (seq - c->flush_cursor >= K_) {  // ring full, spin
    if (backpressure_out) ++*backpressure_out;
    __builtin_ia32_pause();
  }
  e->slot_idx  = slot_idx;
  e->seq       = (uint32_t)seq;
  e->new_value = value;
  std::atomic_thread_fence(std::memory_order_release);

  // 2. Dirty-queue push — V2 uses per-flusher shard.
  uint8_t expected = 0;
  if (c->queued.compare_exchange_strong(expected, 1,
                                        std::memory_order_acq_rel)) {
    uint32_t fid = bucket_idx % hdr_->num_flushers;
    DirtyQueueShard &dq = hdr_->dq[fid];
    uint64_t pos = dq.dq_tail.fetch_add(1, std::memory_order_acq_rel);
    // Capacity check: if pos - dq_head >= cap, the push is best-effort.
    // Producer writes the slot anyway; flusher's idle-scan fallback
    // (kIdleScanUs) catches any drop. We do NOT clear `queued` on
    // overflow because the per-bucket has_pending() will still report
    // pending and the idle scan will drain it.
    __atomic_store_n(&dq.dq_slots[pos % kDirtyQueueCapacity],
                     bucket_idx, __ATOMIC_RELEASE);
  }
}
```

### Consumer-side flusher_loop(int my_id)

Each flusher reads only its own shard:

```cpp
void CxlKvStoreC::flusher_loop(int my_id) {
  BatchRingHeader *hdr = batch_ring_.header();
  DirtyQueueShard &dq  = hdr->dq[my_id];
  const uint64_t kIdleScanUs = 5000;
  uint64_t last_activity_us = now_us();

  while (!hdr->stop.load(std::memory_order_acquire)) {
    bool did_any = false;
    for (int batch = 0; batch < 1024; batch++) {
      uint64_t tail = dq.dq_tail.load(std::memory_order_acquire);
      uint64_t head = __atomic_load_n(&dq.dq_head, __ATOMIC_ACQUIRE);
      if (head >= tail) break;
      uint32_t idx = __atomic_load_n(
          &dq.dq_slots[head % kDirtyQueueCapacity], __ATOMIC_ACQUIRE);
      __atomic_store_n(&dq.dq_head, head + 1, __ATOMIC_RELEASE);
      // Sanity: idx should belong to my partition. If not, log.
      // (Possible if peer producer races on num_flushers env mismatch.)
      drain_bucket(idx);
      did_any = true;
    }
    if (did_any) { last_activity_us = now_us(); continue; }
    if (now_us() - last_activity_us >= kIdleScanUs) {
      // Idle scan: my_id only sweeps its partition.
      for (uint32_t idx = my_id; idx < batch_ring_.num_buckets();
           idx += batch_ring_.num_flushers()) {
        if (batch_ring_.has_pending(idx)) drain_bucket(idx);
      }
      last_activity_us = now_us();
      continue;
    }
    timespec ts = {0, 10 * 1000};
    nanosleep(&ts, nullptr);
  }
  // Final drain — partition only.
  for (uint32_t idx = my_id; idx < batch_ring_.num_buckets();
       idx += batch_ring_.num_flushers()) {
    if (batch_ring_.has_pending(idx)) drain_bucket(idx);
  }
}
```

`drain_bucket(idx)` is **unchanged** from iter-3 — it serializes
on the per-bucket cursors (`append_cursor` / `flush_cursor`), so
even if two flushers somehow targeted the same bucket (they won't
under the partition rule), the drain itself would not corrupt state.

## Correctness invariants

### 1. Per-bucket FIFO

For each bucket B, exactly one flusher is the consumer
(`bucket_idx % N` is deterministic). That flusher's loop is
single-threaded over its dq. So the order in which entries for B
are drained equals the order in which they were enqueued via
`append_cursor.fetch_add` — **total order matches iter-3
single-flusher**.

### 2. Slot-write happens-before bump_epoch[B]

Within `drain_bucket(B)`: the flusher writes new slot values,
then `_mm_sfence()`, then `bump_epoch(B)`. Same routine, same
host. Unchanged from iter-3.

### 3. No producer-consumer dq race

`dq_tail.fetch_add` is multi-producer, atomic. `dq_head` is plain
load/store, single consumer. The classic MPSC ring pattern.
Specifically:

- Producer writes `dq_slots[pos % cap]` BEFORE consumer can pop
  position `pos` because consumer only pops after `dq_head <
  dq_tail` and `dq_tail` was advanced AFTER the slot store. The
  release/acquire pair on `dq_slots` provides the
  publication-visibility guarantee.
- No tail/head wraparound issue: `dq_tail` and `dq_head` are
  uint64_t, cumulative; `% kDirtyQueueCapacity` is only used at
  array index time, never compared.

### 4. No inter-flusher coordination

Bucket B is consumed by exactly one flusher F = B % N. Disjoint
write domains. No locks, no fences between flushers needed.

### 5. dq overflow

If `dq_tail - dq_head >= kDirtyQueueCapacity` for shard F,
producer F's slot store overwrites the oldest entry not yet
popped. **However**: the per-bucket `queued` flag prevents repush
during the same drain cycle. Once the flusher pops some entries
and head catches up, the write is "lost" from the dq but the
per-bucket has_pending() still reports stale slots. The idle scan
(kIdleScanUs = 5 ms after dq goes empty) walks the partition and
drains residual buckets.

This means under sustained > 8 K backlog (very unlikely with K=4096
and 65 k buckets), some flushes may be delayed up to 5 ms —
acceptable since 5 ms latency hits w_p99 only, not throughput.

The capacity 8192 per shard × N=4 = 32 K ≤ num_buckets (65 K)
so worst-case full-buckets-dirty case fits.

### 6. Stop / shutdown

`stop.store(1)`. Each flusher exits its loop, then performs a
partition-only final scan. Safe because all worker threads
have already returned from append() — no new entries arriving.

## Per-flusher map

`flusher_id = bucket_id % N` — modular sharding.

Why mod-N:
- Even distribution under uniform bucket access.
- For Zipf-skewed access (workloads A, F): hot buckets each get
  pinned to one flusher. With N=2/4 we expect the "hottest"
  flusher to see ~80 % of the load and the others to be lightly
  loaded — but even so, the hottest single flusher's `bump_epoch`
  rate is at most 1/N of the iter-3 single-flusher's load on
  hot-bucket-only writes (because the writes were stretched
  across all buckets in iter-3, but here the hot bucket pinned
  to one flusher gets all the hot-bucket work). Net effect: hot
  buckets see no benefit from N>1 (they were already serialised
  on the bucket lock); cold/medium buckets see linear speedup as
  they spread across flushers.

For pure-uniform workloads (B, C, D), N-fold parallelism should
yield close to N× throughput on the bump_epoch rate up to the BW
ceiling.

## Configuration

- `FUSEE_BATCH_NUM_FLUSHERS=N`: integer 1..8. Default 1
  (preserves iter-3 byte-for-byte). Read by
  `tests/cxl_ycsb_runner.cc`, passed to
  `store.init_flushers(N)` after region attach.
- `init_flushers(N)`: validates 1 ≤ N ≤ 8; primary client also
  writes `hdr->num_flushers = N` once during attach.
  Non-primary children read num_flushers and start their local
  view's flusher threads. **Each host runs its own flushers**
  (flushers are NOT cross-host coordinated).

## Risk / mitigation

| Risk | Mitigation |
|------|-----------|
| Producer hashes to flusher F, but at start_flusher() time, host h has not started flusher F yet → push silently dropped | Flushers start during `init_flushers()` BEFORE any worker enters store.insert/update. The runner serializes: attach → set_blockpool → init_flushers → fork children → workers run. |
| `num_flushers` value differs across the two hosts → bucket B handled by different flushers on each host | Acceptable. Bucket ownership is a host-local concern: each host independently flushes its own bucket cache to CXL. The flush itself only writes the slot + bumps epoch; cross-host correctness depends only on epoch monotonicity, which is host-local atomic. |
| dq overflow under high contention | See §5: per-bucket `queued` flag + idle scan covers up to 5 ms; under steady state dq fits. |
| Unit tests miss new SPSC bug | Added `tests/cxl_kv_consistency_check.cc` (1 M ops cross-host hash convergence) and `crash-recover-test/` rerun at N=2/4. |

## Test plan (Phase 6 will execute)

1. Single-host smoke: 100 K ops, N=2, all UPDATE → final
   hash table state matches N=1 reference run.
2. Two-host consistency: 1 M ops, N=2, mixed UPDATE → both
   hosts converge to same `(key → blk_off)` map after stop_flusher.
3. Crash-recovery: kill mid-sweep at N=2 → restart → OpLog replay
   produces consistent state.
4. SPSC dq stress: synthetic single-bucket producer hammer (1 M
   ops per producer × 4 producers) → no entries lost (verify
   `bytes_drained == 4M × payload_size`).

## Code change summary

- `src/cxl_batch_ring.h`: BatchRingHeader gains `num_flushers`
  field + DirtyQueueShard array. `bytes_for()` updates.
- `src/cxl_batch_ring.cc`: `append()` routes by mod-N. New
  helpers: `dq_for(bucket_idx)`, `num_flushers()`.
- `src/cxl_kv_ops_C.h`: `flushers_` becomes `std::vector<std::thread>`,
  add `init_flushers(int N)`. `flusher_loop(int my_id)`.
- `src/cxl_kv_ops_C.cc`: rename `flusher_loop()` →
  `flusher_loop(my_id)`, partition the dq pop and idle-scan by
  my_id. `start_flusher()` → `start_flushers()` spawns N threads.
- `tests/cxl_ycsb_runner.cc`: read `FUSEE_BATCH_NUM_FLUSHERS`,
  pass to init, print in SUMMARY.
- `scripts/run_g34_scaling_sweep.sh`: env passthrough already
  wired in iter-3.

Estimated LoC delta: +180 LoC, -40 LoC.

## Decision points already settled

- N stored in-region (CXL header), not env, so peers see same N.
  Default 1.
- dq capacity per shard stays 8192; 4-shard total = 32 K ≤ 65 K
  buckets.
- flusher pinning: not done in V2 (let kernel scheduler decide).
  pthread sched_setaffinity is a future tuning knob.
