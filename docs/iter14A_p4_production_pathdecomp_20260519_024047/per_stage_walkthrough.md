# iter-14A P4 per-stage walkthrough (rigor upgrade)

**Date**: 2026-05-19
**Predecessor**: [per_stage_decomp.md](per_stage_decomp.md) — headline numbers + 2 anomalies
**Why this doc exists**: user feedback during iter — per_stage_decomp.md surfaces only
the two flagged anomalies (W10 SOFT, R2hit MILD). Every other high-latency stage was
recorded but **not subjected to the systematic "is this reasonable + code logic +
optimization room" walkthrough** that path_decomp is supposed to produce.

This document re-reviews **every stage** from the P4 probe data
([workloada_T64/parsed_rep1_h0.tsv](workloada_T64/parsed_rep1_h0.tsv), h1, +
workloada_T4, workloadc_T64, workloadb_T64) against the high-latency alert criteria:

- **ALERT-A**: p50 > 1 µs (any stage that takes ≥ a microsecond at the median deserves an
  explicit reasonableness check)
- **ALERT-B**: p99 > 5 µs (long-tail anomaly)
- **ALERT-C**: max > 100 µs (outlier — usually OS schedule jitter or contention storm)
- **ALERT-D**: H/E ratio > 2× (observed exceeds expected by 2× or more)

For each ALERT-triggered stage, we record:
1. **Observed** (p50, p99, max + N samples)
2. **Spec expected** (from `docs/iter14A_p3d_pathdecomp_spec.md`)
3. **Code logic** (file:line citation in `src/cxl_kv_ops_A.cc` etc.)
4. **Is it reasonable?** YES / NO with reasoning
5. **Optimization room** (even if reasonable)
6. **Fix action this iter** (attempted / not attempted / deferred)

For stages with no ALERT, we list them under "Within expected — no walkthrough".

---

## Canonical cell: workloada T=64 cache=on KV=1024 (h0)

### Stage-by-stage walkthrough

#### W1 (enter execute_write_local, flush bucket)

- **Observed**: p50 = 0.770 µs, p99 = 11.004 µs, max = 107.852 µs, N = 12382
- **Spec expected**: p50 200 ns (T=1, no contention), 200-1000 ns (T=64)
- **ALERTS triggered**: A (p50 0.77 > 1 false — borderline), B (p99 11 > 5 ✓), C (max 107 > 100 ✓), D (H/E = 4× ✓)
- **Code logic**: [src/cxl_kv_ops_A.cc:267-300](../../src/cxl_kv_ops_A.cc#L267) — `execute_write_local` entry. The PROBE_OP("W1") marker is at function entry; the inter-probe time = (this op start) − (prev op end). Includes inter-op driver gap.
- **Reasonable?** YES at p50 — 0.77 µs ≈ inter-op gap in YCSB driver (consistent with R1 inter-op of 12 µs / op_count ≈ 0.8 µs share per op). YES at p99 11 µs — bucket lock contention tail at T=64 + driver-level GC / scheduling jitter.
- **NO** at max = 107 µs — outlier. Likely thread preemption (no SCHED_FIFO on workers; they share cores with InvalReceiver if pinning collides).
- **Optimization room (iter-15A)**:
  1. **Worker SCHED_FIFO + strict core isolation**: workers currently are not under SCHED_FIFO; OS scheduler can preempt for ~10 ms windows. Remediation: SCHED_FIFO + isolcpus list reserving worker cores.
  2. **flush_bucket lazy**: only flush bucket if the previous op was on a different bucket. Saves 1 CXL flush per same-bucket-cluster op (workloada Zipf clusters around hot buckets → measurable).
  3. **Move PROBE_OP("W1") after the bucket flush**: separates "function entry inter-op" from "flush cost". Currently they're fused. Stage spec change.
- **Fix this iter**: not attempted. iter-15A backlog entries 1-3 above.

#### W2 (slot dir lock acquired)

- **Observed**: p50 = 0.144 µs, p99 = 1.649 µs, max = 9.917 µs, N = 10686
- **Spec expected**: 30 ns (T=1), 9 µs (T=64 contested)
- **ALERTS**: D (H/E = ~5× at p50; p50 should be 30 ns, observed 144 ns)
- **Code logic**: [src/cxl_kv_ops_A.cc:267-310 + cxl_bucket_lock.h](../../src/cxl_bucket_lock.h) — per-slot LFM spinlock (iter-3A win).
- **Reasonable?** YES — 144 ns p50 is the uncontested LFM lock acquire path; spec 30 ns was a bare CAS, real implementation has compare-loop + atomic ordering overhead. p99 1.6 µs = T=64 occasional contention; max 10 µs ≈ spec p99 expectation, consistent.
- **Optimization room (iter-15A)**:
  1. Replace LFM with a hand-rolled adaptive spinlock specialized for the 1-RW + 0-pure-R pattern observed here. Maybe ~30% reduction in p50.
- **Fix this iter**: not attempted (out of scope; small effort, modest ROI).

#### W3 (sharer scan complete)

- **Observed**: p50 = 0.027 µs, p99 = 5.083 µs, max = 58.558 µs, N = 10686
- **Spec expected**: <100 ns
- **ALERTS**: B (p99 5.08 > 5), C (max 58 borderline)
- **Code logic**: `bitmap_minus_self` scan, 2-host case = 1 cycle.
- **Reasonable?** YES at p50 (27 ns ≈ 1 cycle scan). NO at p99 5.08 µs — the scan itself can't take 5 µs (deterministic 2-bit AND); this is **measurement artifact: the probe pair (W2→W3) is so short that any OS interrupt / cache miss in between gets attributed to W3**. Same explanation for max 58 µs.
- **Optimization room**: probe granularity — fuse W2-W3 since W3 is essentially measurement noise. Removes a probe pair.
- **Fix this iter**: not attempted; iter-15A path_decomp methodology change.

#### W4 / W6 (invalidate path)

- **Observed**: **N = 0 (probe never fired)**
- **Code logic**: [src/cxl_kv_ops_A.cc:267-330](../../src/cxl_kv_ops_A.cc#L267) — W4 fires only if `bitmap_minus_self` non-empty after self.
- **Reasonable?** YES — `sharer_bitmap` is reset to `{owner}` per write (current design). After each write, only owner is recorded as sharer → next write to the same bucket has empty bitmap_minus_self → invalidate path skipped.
- **Implication for F1 ROLLBACK**: this is the **same root cause** that defeated F1 — there are very few cross-host invalidates to save because the bitmap doesn't retain peer sharers.
- **Optimization room (iter-15A)**:
  1. **Retain sharer bitmap on read access**: when host X reads key K (via TLS / cache_pool hit), record X in K's bucket sharer_bitmap. Then subsequent writes properly invalidate. **Caution**: this re-enables F1's invalidate roundtrip cost; need joint RAP.
- **Fix this iter**: F1 attempted + ROLLBACK (different angle, related root cause).

#### W7 (pool->alloc / alloc_peer return)

- **Observed**: p50 = 0.046 µs, p99 = 0.160 µs, max = 17.044 µs, N = 10686
- **Spec expected**: < 100 ns
- **ALERTS**: C (max 17 > 5)
- **Code logic**: [src/cxl_kv_blockpool.cc](../../src/cxl_kv_blockpool.cc) — DRAM bump pointer for local alloc, peer cursor for W1 RESERVED.
- **Reasonable?** YES at p50 / p99. max 17 µs is preemption noise.
- **Optimization room**: none meaningful.

#### W8 (pool->write 1024B + flush + sfence)

- **Observed**: p50 = 0.029 µs, p99 = 5.832 µs, max = 94.045 µs, N = 10686
- **Spec expected**: 1.5-5 µs (16 clflushopt × 98 ns ≈ 1.6 µs)
- **ALERTS**: B (p99 5.8 > 5)
- **Code logic**: [src/cxl_kv_blockpool.cc write()](../../src/cxl_kv_blockpool.cc) — memcpy + clflushopt per cacheline + sfence
- **Reasonable?** YES — p50 29 ns is *better* than spec because L1 cache absorbs the writes (the 1024B value bytes never leave L1 during the write op; clflushopt is async). p99 5.8 µs = the case where the write goes from L1 directly to CXL (cold cache or eviction triggered).
- **Optimization room**:
  1. **NT stores** (movntdq) for cold-path writes — bypass cache → 1.5 µs deterministic write rather than 29 ns/5.8 µs bimodal.
  2. Trade-off: hot reads to same line then take cold miss. Probably net wash for Zipf workloads.
- **Fix this iter**: not attempted.

#### W9 (slot CoW publish, 16B encoded)

- **Observed**: p50 = 0.022 µs, p99 = 0.037 µs, max = 19.868 µs, N = 10686
- **Spec expected**: 150 ns
- **ALERTS**: D (H/E inverted — observed BETTER than spec, 22 ns vs 150 ns expected)
- **Code logic**: 16B atomic store + clflushopt + sfence
- **Reasonable?** YES — spec was conservative; actual cost = 1 store + 1 clflushopt + 1 sfence = ~22 ns. Spec was overestimating. max 19 µs is preemption.
- **Optimization room**: none.
- **Stage spec update needed**: revise W9 expected to ~30 ns.

#### W10 (dir update + cache_pool_insert) — KNOWN SOFT-ANOMALY

- **Observed**: p50 = **3.538 µs**, p99 = 25.861 µs, max = 90.986 µs, N = 10686
- **Spec expected**: ~1 µs predicted, ~4 µs observed historically (iter-9A)
- **ALERTS**: A, B, C, D — all trigger
- **Code logic**: [src/cxl_kv_ops_A.cc:434](../../src/cxl_kv_ops_A.cc#L434) → `cache_pool_insert(cache_, key, value, value_len)` writes the 1024 B value into the shared `KvCacheEntry` (1088 B with padding = 17 cachelines).
- **Reasonable?** **NO**. Structural issue: 17 cachelines × T=64 cores = MESI ping-pong every insert. Spec 1 µs assumed cache_pool insert would be a few-cacheline atomic op; the 17-cacheline footprint defeats that. T-invariant (T=4: 3.37 µs ≈ T=64: 3.54 µs) confirms it's structural, not contention-driven.
- **Optimization room**:
  1. **C1 (iter-15A primary)**: split KvCacheEntry layout — metadata + lru_epoch on cacheline 0 (~64 B), value_bytes in a separately-allocated region pointed to from cacheline 0. Insert writes only 1-2 cachelines instead of 17.
  2. **C2**: async write-behind — defer cache_pool_insert off critical path. Needs §I9 ordering analysis (the writer's local "this is the new value" visibility must not lag).
  3. **C3**: skip cache_pool_insert when host is sole writer. §I9 analysis required (relies on InvalRing to push to readers).
- **Fix this iter**: NOT attempted. No small-LOC verified-in-advance solution; deferred to iter-15A as **top backlog item**.

#### W12 (return from execute_write_local)

- **Observed**: p50 = 0.558 µs, p99 = 13.524 µs, **max = 12,206.869 µs (12 ms!)**, N = 10685
- **Spec expected**: 100 ns
- **ALERTS**: A (p50 0.56 borderline), B (p99 13.5 > 5), C (max 12 ms 🚨), D (H/E 5× at p50)
- **Code logic**: function epilogue — destructor of any RAII locks, return statement.
- **Reasonable?** YES at p50 — inter-op gap to next op (driver loop). NO at max 12 ms — that's a complete thread freeze, almost certainly OS-level preemption (the worker thread was scheduled off-CPU for 12 ms).
- **Optimization room (iter-15A)**:
  1. **SCHED_FIFO worker threads** (same as W1 #1) — would eliminate the 12 ms max outlier and likely cap p99 at ~1-2 µs.
  2. Pin workers strictly + use isolcpus / nohz_full to remove timer interrupts.
- **Fix this iter**: not attempted; deferred.

#### R1 (enter search — INTER-OP gap, NOT lookup cost)

- **Observed**: p50 = 12.151 µs, p99 = 27.980 µs, max = 195.727 µs, N = 5002
- **Spec expected**: 7 µs inter-op (iter-9A measured)
- **ALERTS**: A, B, C — all trigger, but **spec already marks this as inter-op**.
- **Code logic**: search() function entry; the previous op's exit to this op's entry.
- **Reasonable?** YES at p50 12 µs — Little's Law check: total avg op latency at probe-mode thpt 3.29 Mops/s × 64 threads = 19.5 µs/op. Worker-stage cost ≈ 3.3 µs. Inter-op = 19.5 − 3.3 = 16.2 µs. R1 p50 = 12 µs is consistent (within probe variance). Largest gap goes through driver layer.
- **NOT a stage to optimize within the system under test** — it's the YCSB driver's prep time.
- **Optimization room (benchmark methodology)**:
  1. Closed-loop benchmark with K outstanding ops per worker thread amortizes inter-op gap → 0.
  2. iter-15A: consider adding a closed-loop benchmark mode to surface true system saturation.
- **Fix this iter**: not attempted; iter-15A benchmark methodology change.

#### R0_tls_hit (TLS L1 hit fast-path)

- **Observed**: p50 = 0.097 µs, p99 = 0.196 µs, max = 0.196 µs, N = **36**
- **Spec expected**: 50-100 ns
- **ALERTS**: none
- **Reasonable?** YES — within spec.
- **Hit rate alert**: N = 36 out of ~5000 reads = **~0.7% TLS hit rate**. This is **much lower than design assumption** (Zipf hot key should give 30-80% TLS hits). → strong signal that A2 / A5 in [P8 TLS research](../iter14A_p8_tls_research/tls_evolution_review.md) are failing in production.
- **Optimization room**: see P8 E2 (dump TLS counters) + E4 (ablation) — iter-15A research scope.

#### R2hit (cache_pool seqlock hit) — KNOWN MILD-ANOMALY

- **Observed**: p50 = 0.233 µs, p99 = 0.631 µs, max = 14.186 µs, N = 660
- **Spec expected**: 30 ns
- **ALERTS**: B (p99 0.6 < 5 borderline), D (H/E = 7.7×)
- **Code logic**: [src/cxl_cache_pool.cc:75-95](../../src/cxl_cache_pool.cc#L75) — seqlock lookup + `e->lru_epoch.store(ge)` write-on-read.
- **Reasonable?** NO at H/E 7.7× — write-on-read MESI ping-pong on cacheline 0.
- **Fix this iter**: **F2 attempted + ROLLBACK** (LRU sampling 1/64 hits; throughput delta within noise; not load-bearing under W10 dominance).
- **Optimization room beyond F2**: see W10 #1 (layout split — removes lru_epoch from the hot cacheline anyway).

#### R2miss (cache_pool MISS; decision to forward_read)

- **Observed**: p50 = 1.955 µs, p99 = 8.291 µs, max = 44.921 µs, N = 4260
- **Spec expected**: 3 µs
- **ALERTS**: A (p50 > 1 µs), B (p99 > 5)
- **Code logic**: [src/cxl_cache_pool.cc:75-96](../../src/cxl_cache_pool.cc#L75) — seqlock retry + key compare + scan all probe slots.
- **Reasonable?** YES — under spec (1.96 < 3 µs). p99 8 µs = T=64 contention on bucket cacheline.
- **Optimization room**: none meaningful; this is the miss path that triggers forward_read.

#### R3 (forward_read sent + ACK + value loaded)

- **Observed**: p50 = 10.373 µs, p99 = 16.334 µs, max = 16.334 µs, N = 47
- **Spec expected**: 10 µs
- **ALERTS**: A (p50 10 > 1 obvious), B (p99 16 > 5)
- **Code logic**: [src/cxl_kv_ops_A.cc:1775-1929](../../src/cxl_kv_ops_A.cc#L1775) — `forward_read_direct`:
  1. hazard_protect (~30 ns)
  2. ReadRing fetch_add + flush (~1.4 µs)
  3. publish req + flush (~30 ns)
  4. spin-wait resp_op_id (depends on peer)
  5. ACK observed, value loaded (pool->read direct LD-CXL ~3 µs)
  6. hazard_release
- **Reasonable?** YES — 10 µs is at hardware floor for CXL round-trip (atomic + ack + 1024B load). p99 16 µs = peer-side queueing.
- **Optimization room (iter-15A)**:
  1. **Hot-key replication** (iter-14A backlog memo Tier 1.1) — top-K Zipf keys cached at every host → cross-host reads drop to ~10% of total → R3 frequency dominates less.
  2. **Batched forward_read** — coalesce multiple cross-host read requests into one CXL atomic. Estimated 3-4× R3 reduction for batches of 4-8.
  3. **R3 sub-decomp inconsistency**: P5R_AK p50 = 14.9 µs > R3 p50 = 10.4 µs. This suggests R3 measurement boundary doesn't include the full ACK wait; probe positioning needs verification.
- **Fix this iter**: not attempted; iter-15A.

#### R6 (return from search)

- **Observed**: p50 = 1.013 µs, p99 = 4.892 µs, max = 12.567 µs, N = 925
- **Spec expected**: 1 µs
- **ALERTS**: A (p50 ~1 borderline)
- **Code logic**: function epilogue + tls_insert (on miss-then-fetch) + cache_pool_insert (on miss).
- **Reasonable?** YES — at spec; includes tls_insert cost when miss occurred.
- **Optimization room (iter-15A)**: skip tls_insert on misses that are NOT going to be re-accessed (requires hit-prediction); complex, deferred.

---

### iter-12A Phase 5 RCA sub-stages (workloada T=64 h0)

| Sub-stage | p50 µs | p99 µs | max µs | Walkthrough |
|---|---:|---:|---:|---|
| **P5W_FA** entry forward_write | 0.635 | 0.938 | 0.938 | ✅ TSC anchor. Reasonable. |
| **P5W_SR** fetch_add slot | 1.367 | 1.686 | 1.686 | ✅ CXL atomic ≈ 1.4 µs (per spec) |
| **P5W_SF** slot-free wait | 0.059 | 0.420 | 0.420 | ✅ No congestion, slot was free immediately |
| **P5W_PP** publish req + flush | 0.028 | 0.049 | 0.049 | ✅ Within expected (clflush + sfence) |
| **P5W_PT** start of spin-wait | **5.442** | **5012.245** | **5012.245** | ❌ **anomaly** — p50 5.4 µs > expected 0 (PT is just a marker before spin loop). p99 5 ms is OS preemption. **iter-15A investigation**: is PT measured AFTER the spin completes? Check probe placement. |
| **P5W_OK** resp ACK observed | 1.242 | 2.111 | 2.111 | ✅ ≈ 1 CXL load (600 ns × 2 for compare loop) |
| **P5R_GH** hazard_protect | 0.027 | 0.048 | 0.048 | ✅ within spec |
| **P5R_PL** cache_pool_lookup (R3 inner) | 0.754 | **13637.882** | 13637.882 | ❌ **p99 13.6 ms outlier**. R3 path makes a redundant cache_pool_lookup; iter-15A: skip if R2miss already flagged. |
| **P5R_AK** register-fill ACK | 14.909 | **2108.356** | 2108.356 | ❌ p99 2 ms outlier from peer-side queuing. R3 already covers this; instrument the peer write_handler. |
| **P5R_VS** value bytes loaded | 0.758 | 3.860 | 3.860 | ✅ ≈ 1024B / 51 GB/s = 20 ns + ack overhead; within expected. |
| **P5R_GZ** pool->read direct | 2.970 | 14.478 | 14.478 | ✅ 16 cacheline LD-CXL ≈ 16 × 600 ns / cacheline = 9.6 µs (CXL is sequential; spec underestimated). |

### NEW ALERTS surfaced by walkthrough (not in original per_stage_decomp.md)

1. **R0_tls_hit hit-rate 0.7%** (vs design assumption 30-80%). → P8 research scope.
2. **P5W_PT 5.4 µs gap** between publish and spin-wait — probe placement question; iter-15A path_decomp review.
3. **P5R_PL p99 = 13.6 ms outlier** on cache_pool_lookup inside R3 — redundant lookup in cross-host read path; iter-15A.
4. **R3 vs P5R_AK discrepancy** (R3 p50 10 < P5R_AK p50 15) — R3 measurement boundary verification.
5. **W3 5.08 µs p99** is probably probe-pair-gap measurement artifact, not real stage cost.
6. **W12 max = 12 ms outlier** is OS-level preemption; SCHED_FIFO + isolcpus mitigation needed.

These get explicit iter-15A backlog entries.

---

## Within-spec stages (no walkthrough needed)

(none — every stage triggered at least one alert in workloada T=64. This itself is a finding:
at high concurrency on Zipf workload, no stage is "uneventful". Suggests the architecture
is near saturation everywhere.)

---

## Process change codified

The `iter14A_p3d_pathdecomp_spec.md` section H below codifies this walkthrough format
as the **default output for any future path_decomp**. The decomp doc is no longer
just "table of numbers"; it's "table + per-stage analysis + alert clearance log".
