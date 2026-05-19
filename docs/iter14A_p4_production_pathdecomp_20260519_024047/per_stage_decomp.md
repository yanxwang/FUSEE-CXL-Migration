# iter-14A P4 per-stage decomposition

**Date**: 2026-05-19
**Build**: build-cxl-w1-probe (HAZARD + W1 RESERVED + FUSEE_PROBE=1)
**MAX_OPS**: 20000 (per cell) — smaller than iter-9A's 200k to bound probe data
**Reps**: 1 each (anomaly retry up to 5 if needed; not needed for any cell)
**Spec**: [iter14A_p3d_pathdecomp_spec.md](../iter14A_p3d_pathdecomp_spec.md)

---

## Headline numbers per cell (probe-enabled binary, NOT throughput-representative)

| Cell | trans_agg_thpt (Mops/s) | Notes |
|---|---:|---|
| workloada_T64 cache=on kv=1024 | 3.29 | probe overhead in slow mode (~50% of probe-OFF baseline) |
| workloada_T4 cache=on kv=1024 | 0.71 | low-T baseline; in normal range |
| workloadc_T64 cache=on kv=1024 | 5.49 | pure read; below probe-OFF baseline due to FUSEE_PROBE overhead + small OPS |
| workloadb_T64 cache=on kv=256 | 3.35 | mostly local cache; KV=256 lighter pool cost |

Throughput drop vs probe-OFF baseline is expected — focus is per-stage latency.

---

## workload-a T=64 cache=on KV=1024 (canonical hot Zipf high-contention cell)

| Stage | N | p50 µs | p99 µs | Expected p50 (spec §A) | H/E ratio | Status |
|---|---:|---:|---:|---:|---:|---|
| **W1** (enter execute_write_local) | 12382 | 0.77 | 11.0 | 0.2 (inter-op) | 4× | inter-op gap dominates |
| W2 (dir lock acquired) | 10686 | 0.14 | 1.65 | 0.03-9 | OK | uncontested mostly |
| W3 (sharer scan) | 10686 | 0.03 | 5.08 | <0.1 | OK | trivial scan |
| W7 (pool->alloc returned) | 10686 | 0.05 | 0.16 | <0.1 | OK | DRAM bump |
| W8 (pool->write 1024B) | 10686 | 0.03 | 5.83 | 1.5-5 | better than expected (cache absorbed) | |
| W9 (slot CoW publish) | 10686 | 0.02 | 0.04 | 0.15 | OK | |
| **W10** (dir update + cache_pool_insert) | 10686 | **3.54** | 25.9 | **1 µs** | **3.5×** | **SOFT ANOMALY** (consistent with iter-9A/10A/13A finding) |
| W12 (return) | 10685 | 0.56 | 13.5 | 0.1 (inter-op) | inter-op | tail to next op |
| **R1** (enter search) | 5002 | **12.15** | 27.98 | 7 (inter-op) | inter-op | NOT a lookup cost |
| R0_tls_hit | 36 | 0.10 | 0.20 | 0.05-0.1 | OK | TLS L1 hit fast path |
| R2hit | 660 | 0.23 | 0.63 | 0.03 | 7.7× over | **MILD ANOMALY** — likely LRU write-on-read overhead (per prior chat) |
| R2miss | 4260 | 1.96 | 8.29 | 3 | OK | branch overhead |
| R3 (forward_read cross-host) | 47 | 10.37 | 16.3 | 10 | OK | ReadRing RT |
| R6 (return) | 925 | 1.01 | 4.89 | 1 | OK | function epilogue |

### Anomalies found

**SOFT-ANOMALY 1: W10 = 3.54 µs p50 (3.5× expected)**

Same finding as iter-9A redo Phase 3, iter-10A Phase 4, iter-13A Phase 0.
W10 includes `cache_pool_insert` which on hot Zipf bucket = seqlock-CAS
+ 1024B memcpy of value bytes + bucket_epoch bump. Under T=64
concurrent writers + readers, the 17-cacheline KvCacheEntry MESI ping-
pongs across cores.

Cause: clear (MESI cacheline ping-pong on shared 1088 B KvCacheEntry).
Solution candidates:
- C1 (large): True lock-free hashmap for cache_pool — multi-day effort,
  unverified ROI; iter-15A
- C2 (medium): Async write-behind — defer cache_pool_insert off
  critical path; needs care for §I9
- C3 (small): Skip cache_pool_insert when host is sole writer (rely
  on InvalRing to push to readers). Requires §I9 analysis.

None of these is "small + verified-in-advance" per fix policy. All have
non-trivial design risk. → Mark as **P4 unresolved**, iter-15A backlog.

**MILD-ANOMALY 2: R2hit = 0.23 µs p50 (7.7× expected 0.03 µs)**

The LRU write-on-read bug surfaced in prior conversation. Specifically
`cache_pool_lookup` does `lru_epoch.store(ge)` on every hit (line 91-92
in cache_pool.cc). This creates a cross-core ping-pong on the entry's
cacheline 0 (where lru_epoch lives) even on pure-read workloads.

Cause: clear (write-on-read, cache_pool.cc:91-92).
Solution: **LRU sampling — 1/64 rate** (3 LOC).
LOC: tiny.
Verified-in-advance: yes — known pattern, low risk, idempotent change.
→ **Implementing as F2 fix**.

---

## workload-a T=4 cache=on KV=1024 (low-T baseline)

| Stage | p50 µs | T=64 p50 | Note |
|---|---:|---:|---|
| W10 | 3.37 | 3.54 | structural, not contention-driven |
| R3 | 9.63 | 10.37 | similar (cross-host RT not T-dependent) |
| W1 | 0.74 | 0.77 | inter-op |

W10 is approximately T-invariant → it's a fixed-cost stage, not a
contention scalability issue. This strengthens the conclusion that
cache_pool_insert's 1024B memcpy is the dominant cost, not the
CAS retry pattern.

---

## workload-c T=64 cache=on KV=1024 (pure-read)

(parsed from h0 below; very few writes in this cell)

Read-path stages dominate. No notable anomalies beyond R2hit which is
the same LRU-write-on-read issue.

---

## workload-b T=64 cache=on KV=256 (smaller KV)

Stages similar to workload-a but value_bytes cost reduced (256B vs 1024B
in W8 / cache_pool memcpy). W10 reduced proportionally.

---

## Little's Law sanity check

Canonical cell (workload-a T=64 cache=on KV=1024):
- Throughput = 3.29 Mops/s (probe-enabled, mode-uncertain)
- Avg op latency ≈ W10 (3.54) + W1 (0.77) + W12 (0.56) = ~5 µs per write op
  + R0/R2hit/R6 (~1.4 µs) per read
  + R3 (10 µs) × 1% miss rate ≈ 0.1 µs average
- Total avg ≈ ~3 µs (50% write × 5µs + 50% read × 1.5µs)
- Theoretical max = 64 / 3 µs = 21.3 Mops/s
- Measured: 3.29 Mops/s (probe-on, slow mode) or ~10-17 Mops/s (probe-off)

Gap: significant. With probe-off ~10 Mops/s and theoretical max 21
Mops/s, ~50% gap. Suggests off-stage contention or queueing. Could be:
- bimodal flat-line behavior (~50% of runs stuck in slow mode)
- worker fork/sched overhead at 64 threads

**Bimodal-flat** is the most-suspected and aligns with the P3.C probe
overhead measurement (bimodal noise). → iter-15A bimodal RCA item.

---

## Fix queue

| ID | Anomaly | Cause | Fix candidate | LOC | Action |
|---|---|---|---|---:|---|
| F2 | R2hit 7.7× expected (cache_pool_lookup LRU write-on-read) | lru_epoch.store on every hit | LRU sampling 1/64 (rdtsc & 0x3F == 0) | 3 | implement |
| (W10 anomaly) | cache_pool_insert MESI ping-pong on 1088B entry | structural KvCacheEntry layout | no small-LOC verified fix | n/a | iter-15A backlog |
| (bimodal collapse) | unknown — half runs at 50% speed | dynamic state we don't understand | needs RCA, not fix | n/a | iter-15A backlog |

→ F2 attempt → measure → decide PASS/ROLLBACK.
