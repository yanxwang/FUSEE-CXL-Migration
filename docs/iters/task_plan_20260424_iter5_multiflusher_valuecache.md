# Task plan — iter-5 — multi-flusher V2 + value-block DRAM cache

**Author**: Claude
**Date drafted**: 2026-04-24
**Status**: DRAFT v1 — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefix `[iter5-mf]` for
multi-flusher work, `[iter5-vcache]` for value-cache work,
`[iter5-mb]` for microbench work)

---

## Open design questions for user (please decide before Phase 1)

| # | Question | Claude default | User position |
|---|----------|----------------|---------------|
| Q1 | Iter-5 scope: do **all 3 workstreams** (microbenches + multi-flusher + value-cache) in one iter, or split into iter-5 (mb + multi-flusher) and iter-6 (value-cache)? | **Split** — multi-flusher alone has 5+ days of careful work; value-cache landing on top of an unstable flusher loses the controlled comparison | ✅ user decided |
| Q2 | Multi-flusher shard count default | **N=2** (lower contention risk; iter-3 saturated at single-thread, doubling already validates the hypothesis) | ✅ user decided |
| Q3 | Multi-flusher: use `pthread` or `boost::fiber`? | **pthread** (boost fibers share a scheduler, multi-fiber flusher would still serialize on the CPU; we want true parallel cores) | ✅ user decided |
| Q4 | Value-cache admission policy | **TinyLFU-style** (frequency-counter sketch + LRU eviction, with all-reads admission). Avoids the "only hot reads" pathology on Zipf long tail; hot reads naturally win the eviction race | ✅ user decided |
| Q5 | Value-cache storage location | **Local DRAM**, separate from bucket cache (different lifecycle, different invalidation source) | ✅ user decided |
| Q6 | Sweep matrix at iter-5 end | **kv ∈ {256, 512, 1024}** × **flusher ∈ {N=1, N=2, N=4}** = **9 sweeps × 80 runs = 720 runs**. Each sweep already runs both cache modes (on/off) per the existing `run_g34_scaling_sweep.sh` infrastructure; no extra axis needed | ✅ user decided |
| Q7 | Iter-5 deadline + autonomy mode | undecided; plan estimates 5–7 days for split option, 10–14 for combined | ⏳ |

Sections 3–8 below assume the **Split** decision (Q1) — iter-5
covers microbenches + multi-flusher only, value-cache deferred to
iter-6. If user chooses combined, append iter-6 sections to iter-5.

---

## 1. Motivation

Iter-4 confirmed two facts and exposed two structural problems.

**Confirmed by iter-4**:
1. Variable value-size pool path works (320 runs, 0 fails).
2. Throughput drops monotonically with vsize, consistent with a
   bandwidth-tied story.

**Exposed structural problems** (these are the iter-5 targets):
1. **Workload A misses 20 Mops/s at every value size** (12–17
   Mops/s). iter-3 phase-3 latency_decomp pinned the cause: single
   flusher saturates on cross-host `bump_epoch` rate at T > 64.
   Value-size tuning cannot fix this — it is a flusher-throughput
   problem.
2. **The "BW-saturated at kv1024" claim used a 22 GB/s ceiling**;
   user has supplied authoritative mlc numbers (51.78 GB/s per-host
   write, 393.22 GB/s DRAM write, 174.8 / 607.8 ns latencies; see
   `docs/design_goals.md`). The earlier ceiling was 2× too tight,
   so iter-4's 0.82-of-ceiling claim is unsafe — the real
   utilisation could be anywhere from 0.17 (pure writer side) to
   0.82 (with 2× cross-host amplification). M1 microbench resolves
   this ambiguity.

The 20 Mops/s north-star bar is still binding (`design_goals.md`).
After iter-5 we want **A ≥ 25 Mops/s on at least one (vsize, T)
cell**, with M-series microbenches on disk to back the claim that
the remaining gap to 30+ Mops/s (if any) is BW-bound, not
flusher-bound.

## 2. Scope

### 2.1 In-scope (iter-5)

**M-series microbenches (P0; ~2 days)**
- M1: dual-host CXL aggregate write BW microbench. Two hosts
  simultaneously stream `clflushopt`-bracketed writes to a shared
  CXL region. Output: aggregate sustained GB/s; reveals whether
  expander supports 2× per-host BW or shares 51.78 GB/s.
- M2: per-op CXL byte-account via uncore counters. Run an existing
  C-only sweep at one (workload, T, vsize) cell with
  `perf stat -e UNC_M_CAS_COUNT` (or equivalent CXL-traffic counter).
  Compare measured bytes/op to theoretical bucket+value account.
  The ratio = cross-host amplification factor (current guess: 1.5–2.0×).
- M3: stage-decomposition sweep on kv256/512/1024 using the
  existing `FUSEE_LATENCY_DECOMP=1` instrumentation (already in
  `src/cxl_kv_ops_C.cc` from iter-3 phase-1). Output: per-stage
  median + p99 latency vs vsize, identifies which sub-stage grows
  fastest with vsize.

**Multi-flusher V2 (P1; ~5 days)**
- SPSC sharded design (no shared dirty queue → no tail race).
- N flusher threads, each owns bucket shard `bucket_id % N`.
- Producers (host worker threads) compute flusher_id at enqueue
  time and push into the matching SPSC ring.
- Each flusher does `dequeue → write_slots → sfence → bump_epoch`
  serially on its shard.
- Configurable via `FUSEE_BATCH_NUM_FLUSHERS` env (already wired
  into the sweep script from iter-3 micro-batch attempt).
- Default N=1 preserves byte-for-byte iter-3 behavior.

**Iter-5-end sweep matrix (P1; ~6 hours wall-clock)**
- **9 × 80-run scaling_ycsb_C_only sweeps** = 720 runs total:
  - kv ∈ {256, 512, 1024} (drop kv=8 from iter-4 — already covered)
  - N ∈ {1, 2, 4} (single-flusher control + 2 / 4 shards)
  - each sweep covers cache=on and cache=off in the same run
- Per-sweep wall: ~35 min × 9 ≈ 5–6 h on g3+g4. Sequence them
  via the same chain script pattern used in iter-4.
- N=1 sweeps at every kv-size give controlled comparison points
  for the multi-flusher delta (vs the iter-4 kv=N=1 numbers,
  re-validated under same code-path).

### 2.2 Out-of-scope (iter-5)

- **Value-block DRAM cache** (deferred to iter-6 per Q1 split). 
- **A/B protocol parity** for variable-KV (still not needed).
- **Crash recovery** of value blocks (still ephemeral pool).
- **Lazy-free GC** (still stub; sweep working set fits within 2 GiB).
- **SCAN / range queries** (out of scope for the entire migration).

## 3. Phased breakdown

Each phase ends with a green checkpoint (compile + smoke run +
sanity check). All commits carry the prefix listed below.

| # | Phase | Prefix | Deliverable | Est | Verify |
|---|-------|--------|-------------|----:|--------|
| 0 | Plan review | — | This doc approved | — | user sign-off |
| 1 | M1 — dual-host BW microbench | `[iter5-mb]` | `tests/cxl_dualhost_bw_bench.cc`, results in `docs/iter5_microbench/m1_dualhost_bw.md` | 0.5 d | aggregate GB/s reported with 2× check |
| 2 | M2 — uncore byte-account | `[iter5-mb]` | run script `scripts/m2_byte_account.sh` + analysis in `docs/iter5_microbench/m2_byte_amplification.md` | 0.5 d | amplification factor for kv256/1024 reported |
| 3 | M3 — stage decomp at vsize | `[iter5-mb]` | regenerate iter-3 stage decomp at kv256/512/1024 + analysis in `docs/iter5_microbench/m3_stage_vs_vsize.md` | 1.0 d | per-stage curve vs vsize |
| 4 | Multi-flusher V2 — design doc | `[iter5-mf]` | `docs/iter5_multiflusher_design.md` (SPSC layout, ordering proof, N-shard map) | 0.5 d | user sign-off |
| 5 | Multi-flusher V2 — code | `[iter5-mf]` | new `src/cxl_dirty_queue_spsc.{h,cc}`, refactor of `src/cxl_kv_ops_C.cc` flusher loop, `FUSEE_BATCH_NUM_FLUSHERS` env wiring | 2.0 d | unit test + smoke 1-host correctness + 2-host smoke |
| 6 | Multi-flusher V2 — correctness battery | `[iter5-mf]` | run `crash-recover-test/` + new linearizability spot-check + 1 M ops × N=1/2/4 cross-host consistency check | 1.0 d | no consistency violation across 5 runs |
| 7 | Iter-5-end sweep | `[iter5-mf]` | 9 × 80-run sweeps (kv ∈ {256,512,1024} × N ∈ {1,2,4}) = 720 runs; chained sequentially overnight | 0.5 d | ok=80 fail=0 each |
| 8 | Analysis + plots + summary | `[iter5-mf]` | `docs/iter5_summary_<ts>.md`, finalize 9 sweep dirs, runs_index update, **3D plot** (kv × N → peak thpt) per workload + multi-flusher-vs-single bar chart | 1.0 d | summary doc complete |

**Total: ~7 days. Range 5–9 days depending on debugging
overhead in Phase 5–6.**

## 4. File-by-file change list

### 4.1 NEW: `tests/cxl_dualhost_bw_bench.cc` (~120 LoC)

Both hosts run this binary simultaneously (cookie-coordinated like
the YCSB runner). Each host streams nontemporal writes to its own
half of a shared 4 GiB CXL region for a fixed wall-clock duration.

Phases:
1. `attach_cxl_region` (existing `cxl_mm.cc` API)
2. Cookie barrier (existing `wait_cookie` pattern)
3. `for (uint64_t off = my_start; off < my_end; off += 64)`:
   `_mm512_stream_si512((__m512i*)(base+off), payload);`
   `if ((off % 4096) == 0) _mm_sfence();`
4. Final `_mm_sfence()`, then report bytes / wall-clock seconds.
5. Report joint: `aggregate = host0_GB + host1_GB`.

**Hypothesis to test**: aggregate ≥ 100 GB/s ⇒ expander supports
2× per-host. aggregate ≈ 51.78 GB/s ⇒ expander is the shared
limit.

### 4.2 NEW: `scripts/m2_byte_account.sh` (~40 LoC)

Wraps a single iter-3-style C-only run with `perf stat -e
uncore_imc/cas_count_write/,uncore_imc/cas_count_read/`. Counts
total CXL bytes (via the uncore IMC counter that targets the CXL
device) over the trans phase; divides by trans_ops to get
bytes/op. Compares against the theoretical bucket+kv account.

Caveats: needs a known mapping from /sys/devices/uncore_*/ names
to the CXL device path. We will discover this in Phase 2; the
script may need a one-time path-discovery step.

### 4.3 NEW: `src/cxl_dirty_queue_spsc.{h,cc}` (~200 LoC)

Per-(producer-thread, flusher) SPSC ring. Producer side:
- `push(uint32_t bucket_id, uint64_t key, uint64_t blk_off)` — no
  blocking; if ring full, falls back to inline non-batched path
  (current iter-3 behavior).

Consumer side:
- `pop_batch(BatchEntry *out, size_t cap)` — returns up to cap
  entries; advances head. SPSC, no CAS, just `release` store on
  head.

Ring layout: power-of-two size, `head` (consumer) and `tail`
(producer) on separate cachelines, payload entries packed
contiguously. Standard SPSC with `std::atomic<uint64_t>`
acquire-release fences.

### 4.4 MODIFY: `src/cxl_kv_ops_C.cc` flusher loop (~80 LoC delta)

Replace single dirty queue + single flusher with N SPSC rings + N
flusher threads. Pseudocode:

```cpp
struct PerFlusher {
  std::thread th;
  std::vector<CxlSpscRing*> input_rings;  // one per producer thread
  std::atomic<bool> stop{false};
};

void CxlKvStoreC::flusher_loop(int my_id) {
  while (!flushers_[my_id].stop.load()) {
    bool any = false;
    for (auto *ring : flushers_[my_id].input_rings) {
      BatchEntry batch[kFlushBatchMax];
      size_t n = ring->pop_batch(batch, kFlushBatchMax);
      if (n == 0) continue;
      any = true;
      flush_slots_to_cxl(batch, n);     // memcpy + clflushopt
      _mm_sfence();
      for (size_t i = 0; i < n; ++i) {
        bump_epoch(batch[i].bucket_id); // CXL atomic
      }
    }
    if (!any) std::this_thread::sleep_for(kFlushIdleSleep);
  }
}

void CxlKvStoreC::enqueue_dirty(...) {
  uint32_t bucket_id = ...;
  int flusher_id = bucket_id % num_flushers_;
  int my_producer = current_thread_id();  // already tracked
  flushers_[flusher_id].input_rings[my_producer]->push(bucket_id, key, blk_off);
}
```

Ordering invariant proof (correctness):
- Per-bucket FIFO: `bucket_id % num_flushers_` is deterministic →
  same bucket always lands in same flusher → same flusher's loop
  is single-threaded → FIFO trivially.
- Slot-write happens-before bump_epoch[B]: same flusher does both,
  with `_mm_sfence()` between. ✓
- No producer-consumer data race: SPSC ring with proper
  release-acquire on head/tail. ✓
- No inter-flusher coordination needed: flushers operate on
  disjoint bucket sets. ✓

### 4.5 MODIFY: `src/cxl_kv_ops_C.h` (~5 LoC)

Add `int num_flushers_ = 1;`, `std::vector<PerFlusher> flushers_;`,
`init_flushers(int N)` / `stop_flushers()` methods.

### 4.6 MODIFY: `tests/cxl_ycsb_runner.cc` (~10 LoC)

Read `FUSEE_BATCH_NUM_FLUSHERS` env (default 1), pass to
`store.init_flushers(N)`. Print the value in SUMMARY.log so we
can audit later.

### 4.7 MODIFY: `scripts/run_g34_scaling_sweep.sh` (already wired
in iter-3, just confirm passthrough still works).

## 5. Verification plan

### 5.1 Unit-level

- `tests/cxl_blockpool_test.cc` re-runs (existing).
- New `tests/cxl_spsc_ring_test.cc`:
  - Single-thread push-pop returns same data.
  - Two-thread (producer + consumer) does not lose / duplicate
    entries under 10 M ops.
  - Ring-full case returns -1 from push (no overwrite).

### 5.2 Correctness (multi-flusher specific)

- 1-host, N=2 flushers, single-bucket workload: result identical
  to N=1.
- 2-host, N=2 flushers, 1 M ops mixed UPDATE: every host's local
  hash table converges to same `(key → blk_off)` map as N=1 run.
- Crash-recovery: `crash-recover-test/` runs to completion with
  N=2 flushers (kill mid-sweep, restart, OpLog replay still
  consistent).

### 5.3 Performance

- M-series microbench results recorded.
- 9-sweep matrix (kv ∈ {256,512,1024} × N ∈ {1,2,4}) expected
  outcomes (commit only after observing):
  - **N=1 control row** (3 sweeps): matches iter-4 kv256/512/1024
    within ±5 % (regression check across rebuild + new SPSC code)
  - **N=2 row** (kv=256/512): workload A peak ≥ 25 Mops/s on at
    least one cell (**success criterion 1**)
  - **N=2 row** (kv=256): workload F peak ≥ 25 Mops/s
    (**success criterion 2**)
  - **N=4 row** (any kv): demonstrates monotonic improvement
    over N=2 OR clearly identifies the diminishing-returns point
    (i.e., per-flusher epoch atomic rate is no longer binding,
    next bottleneck is producer-side enqueue or BW)
  - **kv=1024 any N**: B + F + D aggregate ≥ 60 Mops/s (confirms
    BW-bound regime; multi-flusher doesn't magic-away BW)

If criterion 1 fails, M3 stage-decomp at kv=8 N=2 should reveal
whether the new bottleneck is (a) producer-side enqueue
contention, (b) a different stage we hadn't measured, or (c) a
correctness-cost (e.g., extra sfences for safety we underestimated).

## 6. Risk analysis

| Risk | Likelihood | Mitigation |
|---|---|---|
| SPSC ring bug similar to iter-3 multi-flusher V1 | Med | Phase 4 design doc forces explicit ordering proof before code. Phase 5 unit tests cover the failure mode iter-3 missed (tail vs slot-write race). |
| N=2 flushers introduce new lock contention not measured in iter-3 | Low-Med | N=2 default; if M3 shows producer-side enqueue contention, fall back to per-producer-thread SPSC instead of per-flusher SPSC. |
| BW becomes binding before flusher parallelism pays off (i.e. epoch-bump rate is no longer the bottleneck after multi-flusher) | Med | M1 result tells us upfront; if 2-host aggregate = 51.78 GB/s shared, then at kv=1024 multi-flusher gives ≤ 5 % gain and we should pivot to value-cache-first (iter-6) immediately. |
| Multi-flusher hurts cache=on read-side performance via more aggressive epoch-bump frequency | Low | epoch-bump count is unchanged (still per-bucket); only the rate per wall-clock sec increases, which is exactly the goal. Reader-side refetch cost only matters if reader cycle is shorter than flusher cycle, which it isn't at our T values. |

## 7. Success criteria summary

Iter-5 succeeds when **all** of:
1. M1, M2, M3 results recorded with concrete numbers.
2. Multi-flusher V2 passes correctness battery (Phase 6).
3. Workload A peak ≥ 25 Mops/s at some (kv, T) cell.
4. Workload B/F/D do not regress > 10 % from iter-3 phase-3.
5. Summary doc produced; runs_index updated.

If criterion 3 fails, document the new bottleneck (per M3
post-multi-flusher rerun) and roll the gap to iter-6.

## 8. Out-of-scope follow-ups (queued for iter-6)

- Value-block DRAM cache with TinyLFU-style admission (Q4/Q5).
- Combined sweep with both multi-flusher AND value-cache.
- A/B variable-KV parity (still scope-deferred from iter-4).
- Lazy-free GC (still stub from iter-4).
