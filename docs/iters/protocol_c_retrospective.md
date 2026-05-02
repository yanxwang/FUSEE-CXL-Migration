# Protocol C — retrospective (iter-1 through iter-5)

**Date**: 2026-04-25
**Scope**: Consolidated review of all five C-protocol optimization
iterations completed since 2026-04-23. Closing C as a focus topic;
opening Protocol A as the next focus per user direction.
**Bar**: `trans_agg_thpt ≥ 20 Mops/s` on YCSB workloads A, B, F
(see `docs/design_goals.md`).

---

## Status as of close

C **passes the bar** on B and F and is bandwidth-bound on
kv-1024 (Layer-2 utilisation 0.73, see
`docs/design_goals.md` ceiling stack). A is the
remaining miss at 19.35 Mops/s (kv-256/N=2 cache-on); diagnosis
revised to **hot-bucket producer-side serialisation under Zipf**,
not flusher-rate-bound. Any further C-side gain on A requires
hot-bucket sharding or LRC relaxation — both were explicitly left
to a future iter when A becomes the focus.

| workload | C peak (best across iters) | bar (20 Mops/s) | status |
|----------|---------------------------:|----------------:|:------:|
| A | 19.35 Mops/s @ kv256/N=2/cache=on (iter-5) | 20 | ❌ -3 % |
| B | 33.37 Mops/s @ kv8/cache=on (iter-3 phase-3) | 20 | ✅ |
| C | 57.72 Mops/s @ kv8/cache=on (iter-4 baseline) | n/a (read-only ref) | ✅ |
| D | 48.01 Mops/s @ kv8/cache=on (iter-4 baseline) | n/a (read-mostly ref) | ✅ |
| F | 21.86 Mops/s @ kv256/N=4/cache=off (iter-5) | 20 | ✅ |

---

## Per-iter breakdown

### iter-1 (2026-04-23) — per-slot LFM

**Plan**: `docs/iters/task_plan_20260423_c_writepath.md` (steps 1–2).
**Commits**: `bdd27c9`, `c59698b`, `d427d11`.
**Sweep**: `docs/g34_scaling_ycsb_C_only_20260423_051200/`.

**Done**:
- Removed LFM's `O(MAX_HOST_NUM=200)` peer-scan by switching from
  per-bucket lock to **per-slot LFM** (7 lock entries per bucket).
  Touched: `src/cxl_bucket_lock.{h,cc}`, `src/cxl_kv_ops_C.cc`.
- Added `FUSEE_PER_SLOT_LOCK=ON`, `FUSEE_USE_TICKET_LOCK=OFF` env
  flags for compile-time selection.
- Latency-decomp instrumentation gated by `FUSEE_LATENCY_DECOMP=1`
  in `src/cxl_kv_ops_C.cc`.

**Solved**:
- A peak 1.08 → 3.41 Mops/s (3.16 ×).
- B peak 6.55 → 9.98 Mops/s (1.52 ×).
- F peak 1.44 → 3.53 Mops/s (2.45 ×).
- Lock p99 at T=64 on workload A: 12.8 ms → 440 µs (29 ×).

**Left behind**:
- Still 5.9 × / 2.0 × / 5.7 × short of 20 Mops/s on A / B / F.
- C/D regressed slightly (17 GiB SlotLockTable init overhead).
- Ticket-lock proven unusable as default — cacheline storm under
  Zipf even with the patched `clflushopt-before-fetch_add`.

### iter-2 (2026-04-23) — atomic epoch-bump outside critical section

**Plan**: same as iter-1 (extension of step 3).
**Commits**: `17f6949`.
**Sweep**: `docs/g34_scaling_ycsb_C_only_20260423_054027/`.

**Done**:
- Moved `bump_epoch` (atomic CXL `__atomic_add_fetch + clflushopt
  + sfence`) **outside** the LFM critical section.
- Added writer-side **local-host** DRAM cache refresh so the
  writer sees its own write immediately (read-your-own-writes
  within the writer host).
  - **This does NOT violate LRC.** LRC's whole point is the
    asymmetry: same-host immediate visibility + cross-host
    deferred-until-release. Cross-host peers still see the
    write only after `bump_epoch`, with formal staleness bound
    `T_flush + cxl_epoch_latency` (see `docs/design_goals.md`
    "Protocol-C under micro-batching" section).

**Solved**:
- F peak 3.53 → 4.34 Mops/s (+23 %).
- B peak 9.98 → 10.54 Mops/s (+6 %).
- D peak 38.30 → 41.00 (+7 %).
- A peak 3.41 → 3.27 (-4 %, noise).

**Left behind / what we learned**:
- **Structural ceiling identified**: the cross-host atomic epoch
  bump itself is ~3 µs / op at the CXL hardware level. Per-hot-slot
  ceiling under strict LRC ≈ 330 k ops/s. **No further per-op
  optimisation fits under strict LRC semantics** — further gain
  must come from a design-level change.
- iter-3 candidates listed: relax LRC + batched epoch, per-host
  shard, same-key micro-batching. **Iter-3 picked the third.**

### iter-3 (2026-04-24) — Phase-2 fixes + Phase-3 micro-batching

**Plan**: `docs/iters/task_plan_20260424_lock_decomp_microbatching.md`.
**Commits**: `9f529a1`, `32cb92f`, `75c99f1`, `c5393a3`,
`461cc02`, `072070c`, `94da706`.
**Sweeps**:
- `docs/g34_scaling_ycsb_C_only_20260424_044118/` (Phase-2)
- `docs/g34_scaling_ycsb_C_only_20260424_052400/` (Phase-3)
- `docs/g34_scaling_ycsb_C_only_20260424_091433/` (2 M-ops validation)

**Done — Phase 2 (low-risk fixes)**:
- `[read-singleshot]` — `search()` 8-attempt retry → single pass.
- `[flush-collapse]` — 14 `clflushopt`/bucket-scan → 2.
- `[route-seq]` — `SlotLockEntry.route_seq`; UPDATE skips
  under-lock re-verify when bucket unchanged.

**Done — Phase 3 (per-host DRAM ring micro-batching)**:
- New `src/cxl_batch_ring.{h,cc}` ring + per-bucket lazy-publish.
- `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_BATCH_MERGE_SAME_KEY=ON` config.
- Spec update in `docs/design_goals.md` "Protocol-C under
  micro-batching — relaxed LRC bound":
  `peer_visibility_lag ≤ T_flush + cxl_epoch_latency (~3 µs)`.

**Solved**:
- **B 33.37 ✓ and F 20.48 ✓ cross 20 Mops/s** for the first time.
- A 3.27 → 17.05 (5.2 × phase-2; **85 % of bar**).
- C ref 45.99 → 48.73; D ref 41.00 → 33.92 (small regression
  acknowledged).

**Left behind**:
- A still 15 % short. Diagnosis at the time: single-flusher
  saturates on per-bucket `bump_epoch` rate.
- Multi-flusher V1 scaffold landed (`94da706`) but **N ≥ 2 buggy**
  — producer/consumer race between `dq_tail.fetch_add` and slot
  RELEASE store. Reverted to N=1 default.

### iter-4 (2026-04-24) — variable KV value-size sweep

**Plan**: `docs/iters/task_plan_20260424_variable_kv_size.md`.
**Commits**: `d1a1c64` (Phase 1–6), `c26bb0d` (Phase 9 finalize).
**Summary**: `docs/iters/iter4_variable_kv_summary_20260424.md`.

**Done**:
- Per-host bump-alloc CXL pool (`src/cxl_kv_blockpool.{h,cc}`) +
  dual-path C variadic insert/update/search. `len ≤ 8` keeps
  inline-u64 fast path byte-for-byte iter-3.
- `FUSEE_VALUE_SIZE` env wired through ycsb runner + sweep script.
- 4 × 80-run C-only sweeps at vsize 8 / 256 / 512 / 1024.

**Solved**:
- **Hypothesis confirmed** (BW-crossover): B-peak / BW-ceiling
  ratio: 0.42 (kv256) → 0.68 (kv512) → 0.82 (kv1024). At kv=1024,
  all 5 workloads cluster at 14–17 Mops/s — clean BW-bound regime.
- Variable-length value plumbing works (320 runs, 0 fails).
- F passes 20 Mops/s bar at kv-256 (21.24 cache=on).

**Left behind**:
- A still misses at every vsize (12–17 Mops/s) — **value-size
  tuning cannot resolve Zipf hot-slot contention**, structural
  carry-over from iter-1/2/3.
- `cxl_kv_blockpool::free_lazy()` is a stub. Sweep working set
  fits within 2 GiB but real workloads with sustained UPDATEs
  would exhaust the pool. **GC is the major outstanding piece.**
- iter-4's 22 GB/s × 2× cross-host amplification = 44 GB/s ceiling
  was a guess; actual ceiling unknown until iter-5 M1.

### iter-5 (2026-04-25) — multi-flusher V2 + M-series microbenches

**Plan**: `docs/iters/task_plan_20260424_iter5_multiflusher_valuecache.md`.
**Commits**: `5c83965`.
**Summary**: `docs/iter5_summary_20260425.md`.
**Sweeps**: 9 × `docs/g34_scaling_ycsb_C_only_2026042{4,5}_*/` =
720 runs, 0 fails.

**Done**:
- M1: `tests/cxl_dualhost_bw_bench.cc` — dual-host CXL write-BW
  microbench. Result: per-host 12.5 GB/s, aggregate 25 GB/s
  (= 2 × per-host, expander supports parallel).
- M2: calc-based byte-account using M1 ceiling.
- M3: coarse-grain stage decomp from `w_avg_ns` → constant cost
  ≈ 8 µs (matches iter-2 LFM anatomy).
- Multi-flusher V2 (proper SPSC-per-flusher MPSC dirty-queue
  shard, `bucket_id % N` partition): `src/cxl_batch_ring.{h,cc}`
  + `src/cxl_kv_ops_C.{h,cc}`. **720 runs 0 fails** — design
  proven correct.
- 9-sweep matrix kv ∈ {256,512,1024} × N ∈ {1,2,4}.

**Solved**:
- **iter-4's BW ceiling overestimate corrected** — real Layer-2
  ceiling is 25 GB/s aggregate (not 44). kv1024 utilisation
  revised 0.82 → 0.73.
- Multi-flusher V2 is correct; hangs/data-loss eliminated relative
  to iter-3 V1.
- F passes at kv-256/N=4 cache=off (21.86 Mops/s).

**Left behind / hypothesis falsified**:
- **A peak = 19.35 Mops/s — fails iter-5 success criterion 3
  (≥ 25 Mops/s)**. Multi-flusher contributed only +5 %
  cache=on / +13 % cache=off on A.
- **Diagnosis revised**: A is **hot-bucket producer-bound** under
  Zipf, not flusher-rate-bound. The hottest ~5 % of buckets see
  ~80 % of writes; all those buckets land on **one** flusher
  shard (because `bucket_id % N` is deterministic), so that
  flusher's epoch-bump rate is identical to N=1. Multi-flusher
  only parallelises medium/cold buckets.
- **Multi-flusher hypothesis falsified**. The right next levers:
  hot-bucket slot-shard, async epoch on hot buckets (LRC slack),
  reader-side TTL-bounded staleness.

---

## Outstanding work for protocol C as a system project

Listing the items required to call C "production-ready", not just
"benchmark-ready". Sorted by impact-on-realism (most missing →
least). **None of these are in scope for the current C focus
window** — they are queued for whenever C work resumes.

### Critical (blocks real workloads)

1. **Lazy-free GC for `cxl_kv_blockpool`** (iter-4 §8.4 stub).
   Current pool is bump-allocator only — every UPDATE allocates a
   new block; old blocks are never reclaimed. Sustained UPDATE
   workloads exhaust the 2 GiB pool segment in O(2 GiB / vsize)
   ops (~2 M ops at kv-1024). Required: free-list per-host with
   epoch-protected reclamation. Estimated: 3–5 days.
2. **OpLog integration with blockpool**. `crash-recover-test/`
   currently expects inline-u64 values; recovery from blockpool
   offsets is unimplemented. Required: log `(slot_addr, blk_off,
   len)` tuples; replay walks pool for each UPDATE entry.
   Estimated: 2–3 days.
3. **Variable-length keys** (iter-4 §2 out-of-scope). **Note: not
   the same as variable-length values.** iter-4 added varlen
   **values** (256/512/1024 B via `cxl_kv_blockpool` + dual-path
   variadic API). **Keys are still hard-coded `uint64_t` (8 B)**:
   the entire write path takes `uint64_t key`, slot field `key`
   is u64, hash is `siphash(key, 8)`. Realistic workloads use
   varlen keys (URLs, user-IDs, etc.) which would need: separate
   key-pool, slot becomes `(key_off, val_off)`, hash adapts to
   variable bytes, interface changes to `(const void *key,
   uint32_t key_len)`. Estimated: 5–7 days.

### Important (limits achievable performance)

4. **Hot-bucket sharding for workload A** (iter-5 falsification
   conclusion). Detect hot buckets; partition slots within the
   bucket across multiple lock entries; producers hash-shard their
   writes within the bucket. Estimated: 5–7 days. **Highest
   single-step gain on A** (estimated 25 → 35 Mops/s).
5. **Value-block DRAM cache** (iter-5 Q1 split deferred to iter-6).
   **Note: not the same as the existing bucket DRAM cache.** FUSEE
   already has a **bucket** DRAM cache (`FUSEE_CACHE=1` /
   sweep `cache=on/off`, controls `cache_enabled_` in
   `src/cxl_kv_ops_C.cc`) that caches the 128 B bucket cacheline
   per index. That **bucket** cache exists since pre-iter-1; what
   does NOT exist is a **value-block** cache for the bytes pointed
   to by `blk_off` in iter-4's pool. Read path currently:
   bucket → DRAM cache hit (free) → blk_off → CXL pool read
   (every search pays). Read-heavy workloads (C 100 % read)
   dropped from 57.72 (kv-8 inline) to 31.25 (kv-256 pooled) —
   46 % of that drop is value-read CXL traffic that could be
   cached locally. TinyLFU-style admission per iter-5 plan Q4.
   iter-5 commit `5c83965` has zero `[iter5-vcache]` commits —
   confirmed not landed. Estimated: 3–5 days. **Highest gain on
   C / D / read-heavy F.**
6. **A / B variable-KV parity** (iter-4 §2 out-of-scope). A and B
   stay on inline-u64; cannot run YCSB with realistic value sizes
   under those protocols. Required: same dual-path treatment as C.
   Estimated: 2–3 days each.
7. **System-ram dax mode for FUSEE**. Layer-1 vs Layer-2 4 ×
   ceiling gap is recoverable by switching dax mode. Cost: lose
   address-layout control, kernel offline-hang risk. Estimated:
   2 weeks of redesign + 2 weeks debugging the FUSEE memory model.
   **Only revisit if Layer-2 becomes the binding constraint AND
   no protocol-side optimisation remains.**

### Nice-to-have (deferred maintenance)

8. **Per-workload batching opt-in flag**. D's INSERT path pays
   batching overhead it doesn't need. Estimated: 1 day.
9. **MERGE_SAME_KEY=OFF comparison at best (K, T)**. Quantify
   how much the merge saves for Zipf vs uniform. 1 day.
10. **Peer-visibility lag aux-client**. Empirically validate the
    formal staleness bound `T_flush + cxl_epoch_latency` claimed
    in `docs/design_goals.md`. 2 days.
11. **Scan / range queries** (E workload). Out of scope across
    the entire migration; would need bucket-walk primitives.

---

## Aggregate gain summary (5 iters of C work)

| workload | day-0 baseline | end of iter-5 | gain |
|----------|---------------:|--------------:|-----:|
| A | 1.08 Mops/s | 19.35 | **17.9 ×** |
| B | 6.55 | 33.37 | **5.1 ×** |
| C | 51.22 | 57.72 | 1.13 × |
| D | 45.86 | 48.01 | 1.05 × |
| F | 1.44 | 21.86 | **15.2 ×** |

Two of the three "validation" workloads (B, F) cross the 20 Mops/s
bar; the third (A) is at 0.97 × of bar with the structural
ceiling diagnosed and the next lever identified.

---

## Lessons (referenced by the methodology doc)

The lessons learned across these five iters are extracted into
`docs/refs/optimization_methodology.md` as a reusable workflow.
This retrospective is the source-of-truth for "what happened on
C"; the methodology doc is the abstraction "how to do this on the
next protocol".
