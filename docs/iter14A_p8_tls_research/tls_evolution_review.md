# P8 TLS / shared cache research review

**Phase**: P8 (research only — no code ship)
**Scope**: trace the design evolution from single shared `cache_pool` →
two-tier (TLS + shared `cache_pool`), recover the observation chain that
motivated each step, propose experiments to revalidate the reasoning,
and identify open questions for future iters.

This document is **not** a redesign proposal. It surfaces the assumed
mental model under which the current 2-tier cache lives, so the user
can design clean experiments to verify or falsify each link.

---

## Timeline of the cache layer evolution

| Iter | What landed | Reason given | Source |
|---|---|---|---|
| iter-1A→4A | Single shared `KvCachePool` only (mmap MAP_SHARED arena, MAP_SHARED across workers) | Each worker can see invalidates from peer workers without IPC. Lookup = single bucket scan + memcpy of value_bytes. | iter-4A Phase 3 |
| iter-9A redo Phase 3 | Decomp at workloada T=64 KV=1024 cache=on: **R1 = 7.16 µs p50** dominated by `cache_pool_lookup`'s 1024 B `memcpy(value_bytes)`. The 1048 B `KvCacheEntry` is 17 cachelines; under Zipf hot key + 32 concurrent inserters every reader fetches all 16 data cachelines cross-core (MESI bouncing). | Performance pathology of shared cache_pool under contention. | `cxl_tls_cache.h:13-20` comment |
| iter-10A Phase 1.A-D | **TLS cache layer added above shared cache_pool.** Per-worker (NOT MAP_SHARED, NOT mmap'd, NOT in CXL). Hot Zipf keys cached locally. Coherence via per-bucket `epoch` in shared `cache_pool` — TLS hit reloads bucket epoch + compares before returning value. Mismatch → evict TLS entry + fall through. | Avoid 17-cacheline MESI bounce on every read; TLS hit ≈ 50-100 ns target vs 7160 ns observed. | `cxl_tls_cache.h:1-49`, iter-10A commit log |
| iter-10A Phase 2 | Replaced shared cache_pool spinlock with seqlock-CAS for lock-free reads. | Reduce concurrent insert/lookup interference. | `cxl_cache_pool.cc:98-152` |
| iter-13A Phase 1 | HAZARD pointer direct-pool read (eliminate forward_read staging copy). | Phase 1 RAP — cross-host READ data copy reduction (forward_read pool→staging→DRAM → direct pool→DRAM). | `cxl_read_guard.h:6-34` |
| iter-13A Phase 2 | W1 RESERVED per-host write segments (eliminate forward_write staging copy). | Phase 2 RAP — cross-host WRITE data copy reduction (worker → forward_staging → pool → bucket → worker → pool → bucket). | `cxl_read_guard.h:53-73` |

## What the TLS cache currently looks like

- **Layout**: `TlsCache` = struct of `num_entries × TlsCacheEntry` slots.
  Each entry = 8B key + 8B observed_epoch + 4B value_size + 4B pad + 1024B value_bytes.
  Slot alignas(64) → 1088 B per entry. (Same width as `KvCacheEntry`.)
- **Memory location**: posix_memalign(64, ...) — **DRAM, per-process**,
  not shared, not in CXL devdax.
- **Hashing**: open addressing, linear probe up to kTlsProbeMax = 8 slots.
- **Eviction policy**: bump-cursor "hash-position-replace" → simple LRU
  surrogate. No formal LRU sampling cost (the F2 R2hit anomaly was on the
  shared `cache_pool`, not TLS).
- **Coherence**: bucket-epoch trick (§I9 strict-A linearizability).
  Every CACHE_POOL bucket has a `std::atomic<uint64_t> epoch`. Insert /
  evict / set_stale on cache_pool bump the bucket epoch. TLS reader does:

  ```
  load tls_entry → load shared bucket_epoch (atomic acquire)
  if tls_entry.observed_epoch != bucket_epoch → evict + fallthrough
  else → return tls_entry.value
  ```

- **Sizing**: env-tunable `FUSEE_TLS_SIZE` (default 1024). At T=64 worker
  count × 8192 entries = ~570 MiB total DRAM (g3/g4 96 GB → 0.6%).

## The assumption chain that the design rests on

To produce throughput gain, the TLS cache must satisfy each of these
links simultaneously:

| # | Assumption | What if it fails |
|---|---|---|
| **A1** | The dominant read cost in the single-cache_pool baseline is the 17-cacheline memcpy under MESI bouncing. | TLS adds cost without removing the bottleneck. |
| **A2** | Hot Zipf keys are concentrated enough that a small per-worker TLS holds them. (Workload-a/b/c/d/f Zipf assumes ~80/20 → top 20% of keys = 80% of traffic.) | TLS hit rate is too low; most lookups still hit shared cache_pool. |
| **A3** | Bucket epoch reload cost (1 atomic acquire load on a single cacheline) is much less than the saved 17-cacheline memcpy. | TLS check overhead approaches the saved cost. |
| **A4** | bucket_epoch ping-pong itself is benign — single cacheline, small footprint. | bucket_epoch becomes the new pingpong line. |
| **A5** | TLS staleness (epoch mismatch evictions) is rare enough not to dominate. | High eviction rate → most TLS hits are stale → fallthrough is normal case. |
| **A6** | The shared `cache_pool` is still load-bearing for non-hot keys (TLS misses) at acceptable cost. | If shared lookups are still the bottleneck for misses, TLS doesn't unblock throughput. |

## Indirect evidence in current data

What we can already see in iter-13A / iter-14A P4 data:

- **W10 anomaly** (P4 per_stage_decomp.md, iter-14A): cache_pool_insert
  is 3.54 µs p50 (3.5× spec). The 1088 B KvCacheEntry MESI ping-pongs
  across cores at high T. → **A4 is partially failing** — even with TLS
  above, the shared cache_pool's write path (insert) still has the MESI
  problem on the same 1088 B entry.
- **R2hit anomaly** (P4): cache_pool_lookup hit path is 0.23 µs p50 (7.7×
  spec). Even WITH TLS in front, the shared cache_pool's lookup-hit
  pattern shows write-on-read of `lru_epoch`. → suggests significant
  TLS-miss-then-hit traffic still flows through shared cache_pool.
- **R0_tls_hit** (P4): TLS L0 hit fast path = 0.10 µs p50, 36 samples
  among ~5000 reads → TLS hit rate appears very low (< 1%) in workloada
  T=64. → **A2 is questionable in production data** — either the TLS is
  badly sized, or the bucket-epoch mismatch eviction rate is high, or
  the workload generator's Zipf isn't producing the expected concentration.

## Proposed experiments to revalidate the design

The user explicitly wants to **redesign experiments to verify the
problem → analysis → solution reasoning chain**. Below is a research
plan (not yet executed):

### E1 — Reproduce the "shared cache_pool only" baseline

**Goal**: confirm that single-tier shared cache_pool exhibits the
17-cacheline memcpy MESI bottleneck under workloada Zipf T=64 KV=1024.

**Approach**: build with a `FUSEE_NO_TLS=1` flag (or just bypass tls_lookup
unconditionally) so reads go straight to shared cache_pool. Run
path_decomp with PROBE_OP on read-path. Expected: R2hit = 7+ µs p50
(matches iter-9A redo Phase 3 number), validating A1.

### E2 — TLS hit rate diagnosis

**Goal**: measure TLS hit-rate, miss-rate, epoch-eviction-rate, and
replacement-rate at workloada T=64. The TlsCache struct already has
the four counters (`hits`, `misses`, `epoch_evictions`, `replacements`);
just dump them at run-end per worker thread.

**Approach**: add `FUSEE_DUMP_TLS_COUNTERS=1` at end-of-test path → log
all 64 workers' counters. Run on workloada/c/d/f. Expected: workloadc
(read-only Zipf) should have high TLS hit rate (~70%+); workloada
mixed-RW should have moderate (~40%+). If observed rates are << expected,
A2 is failing.

### E3 — bucket_epoch ping-pong measurement

**Goal**: quantify how often bucket_epoch atomic loads cross cores
(i.e., the TLS check cost). Estimate via `perf c2c` on the bucket_epoch
cachelines during a workloada run.

**Approach**: short run with perf c2c collecting HITM events on the
KvCachePool buckets array address range. Expected: if A4 holds, the
HITM rate on bucket_epoch lines should be much less than HITM on
KvCacheEntry data lines.

### E4 — Cache-pool-vs-TLS ablation matrix

**Goal**: separate contributions of each cache layer.

| Build | tls_lookup enabled | cache_pool_lookup enabled | path |
|---|---|---|---|
| **B0** baseline | yes | yes | (current production) |
| **B1 TLS-bypass** | no | yes | reads always hit shared cache_pool |
| **B2 cache_pool-bypass** | yes | no (cache miss → forward) | TLS-only; misses go to peer |
| **B3 both-bypass** | no | no | no cache, every read forwards |

Run all 4 builds on workloada/c/d cells (T=64 cache=on). Expected
ordering of thpt: B0 > B1, B0 > B2, B0 > B3. If B1 ≈ B0, TLS is
delivering zero throughput. If B2 ≈ B0, shared cache_pool is dead
weight (might want to remove or shrink it).

### E5 — KvCacheEntry layout shrink

**Goal**: test the hypothesis that the 1088 B entry is the structural
problem (W10), independent of TLS. Split data and metadata into
separate cachelines; lru_epoch on its own line; value_bytes on a
separately-allocated chunk.

This **is** a code change, so deferred from P8 (research only) but
captured here for iter-15A backlog.

### E6 — Bimodal-vs-cache interaction

**Goal**: P3.C and P5 both observed bimodal slow/fast. Test whether the
slow mode is correlated with high TLS-eviction rate (initialization
state where bucket_epoch is churned).

**Approach**: run 20 reps of workloada T=64; log per-run TLS counters +
trans_agg_thpt. Plot scatter of (eviction_rate vs thpt). Hypothesis:
slow-mode reps have higher eviction_rate.

### E7 — Coherence stress test

**Goal**: verify §I9 linearizability holds under TLS cache (hash-diff is
the production gate, but stress-test for rare races).

**Approach**: rw_race_test extended to 100k concurrent ops with hash-diff
after each batch. If TLS coherence breaks, hash-diff catches it.

---

## Open questions for the user (to be raised after P9 summary)

1. **Are A2/A5 directly measurable in current production builds?** The
   counters exist (`hits`, `misses`, `epoch_evictions`, `replacements`)
   but there's no logging path that dumps them. Adding the dump is a
   < 10-LOC change (research scope, P8 forbids ship). Should iter-15A
   include a flag to dump these counters? → likely yes.

2. **Should TLS be re-architected as a single-cacheline-entry layout?**
   At 1088 B per entry × 1024 entries × 64 threads, the TLS memory
   footprint is fine; but if value_bytes were heap-allocated separately,
   the per-entry size drops to ~24 B and the bump-replace LRU is much
   cleaner.

3. **Is the bucket-epoch coherence trick actually load-bearing?**
   On a TLS hit, we still read a CXL atomic (bucket_epoch lives in
   shared cache_pool which is in CXL). That's ~600 ns per hit per
   the iter-3A hardware baseline (CXL load latency). If TLS hit cost
   is 600 ns + memcpy 50 ns = 650 ns, is that actually better than
   the 7160 ns iter-9A baseline? Yes, but the gap is smaller than
   the "50-100 ns target" the design assumed.

4. **Does the slow-mode bimodal originate in TLS?** P5 showed slow-mode
   B/op identical between STAGING (no copy-elim) and HAZARD+W1 (full
   copy-elim). One hypothesis: slow mode is high-eviction TLS state
   where most reads end up hitting shared cache_pool (defeating both
   TLS and copy-elim).

---

## Conclusion (research deliverable)

The current 2-tier cache (TLS over shared cache_pool) rests on 6 chained
assumptions (A1-A6) of which:
- A1 and A3 are well-supported by iter-9A redo Phase 3 decomp.
- A2, A4, A5 lack direct in-production measurement (counters exist,
  not dumped).
- A6 is structurally questionable — W10 P4 finding shows shared
  cache_pool's insert path remains the MESI-dominant stage.

The most-likely root cause of "copy elim didn't move thpt" (P5 case B
in slow mode) is that **the bottleneck has migrated from the
forward-path memcpys (iter-13A targeted) to cache_pool_insert's
17-cacheline MESI ping-pong (W10) AND the bimodal slow-mode state**.

iter-15A research-scope additions (no code ship in iter-14A):
- E2: dump TLS counters per worker at end-of-test.
- E4: 4-build ablation matrix (B0/B1/B2/B3).
- E6: bimodal vs eviction-rate correlation.

iter-15A code-scope candidates:
- W10 structural fix (KvCacheEntry layout split).
- Bimodal RCA + flat-line escape mechanism.
- (Lower priority) TLS sizing sweep against workload Zipf parameters.
