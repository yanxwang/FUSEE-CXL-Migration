# iter-14A path_decomp stage specification

**Date**: 2026-05-19
**Architecture under test**: iter-13A HEAD (post-P2 rollback) =
HAZARD read guard + W1 RESERVED write alloc + 6 named/pinned receivers
+ 3 named/pinned senders.
**Baseline primitives**: `docs/path_decomp_iter9A_pre_20260510_060841/baseline.md`
**Supersedes**: `docs/path_decomp_iter9A_*/per_stage_decomp.md` (iter-9A
architecture); `docs/path_decomp_iter12A_*/` (P5W/P5R micro-stages stay
valid as sub-decomp).

---

## A. Top-level stages (use these for the per_stage_decomp table)

### Write path (worker insert/update)

| Stage | Probe tag | What it measures | Expected p50 (T=1, no contention) | Expected p50 (T=64, contention) | Source |
|---|---|---|---|---|---|
| W1 | PROBE_OP("W1") | enter `execute_write_local`; flush bucket | 200 ns | 200-1000 ns | bucket scan ~200ns inter-op gap dominates |
| W2 | PROBE_OP("W2") | acquired slot dir lock | 30 ns (uncontested) | 9 µs (T=64 contested) | spinlock |
| W3 | PROBE_OP("W3") | sharer bitmap scan complete | <100 ns | <100 ns | linear scan over (num_hosts) bits |
| W4 | PROBE_OP("W4") | first invalidate started (only if bitmap non-empty after self) | 1.4 µs | 1.4 µs+ | CXL atomic fetch_add InvalRing.tail |
| W6 | PROBE_OP("W6") | last invalidate ACK observed | depends on N sharers × inval RT (~5 µs each) | same | InvalRing RT |
| W7 | PROBE_OP("W7") | pool->alloc returned (local) OR alloc_peer (W1 RESERVED) | <100 ns | <100 ns | DRAM bump |
| W8 | PROBE_OP("W8") | pool->write 1024B + flush + sfence | 1.5-5 µs | similar | 16 cachelines × clflushopt 98ns ≈ 1.6 µs (cache absorbed lower) |
| W9 | PROBE_OP("W9") | slot CoW publish (16B encoded) | 150 ns | similar | clflushopt + sfence + ST-CXL |
| W10 | PROBE_OP("W10") | dir state updated + cache_pool_insert | **~1 µs predicted, observed 4 µs** | similar | iter-9A H/E=4× soft anomaly; iter-10A CAS made it WORSE (4-6 µs); P4 investigation target |
| W12 | PROBE_OP("W12") | return from execute_write_local | 100 ns | 100 ns | function epilogue |

### Read path (worker search)

| Stage | Probe tag | What it measures | Expected p50 | Source |
|---|---|---|---|---|
| **R0_tls_hit** | PROBE_OP("R0_tls_hit") | TLS L1 hit fast-path returned | 50-100 ns | iter-10A TLS layer; per-thread DRAM hashmap; bucket_epoch atomic check |
| R1 | PROBE_OP("R1") | enter search() — INTER-OP gap; not lookup cost | 7 µs (iter-9A measured) | inter-op (NOT a stage cost!) |
| R2hit | PROBE_OP("R2hit") | shared cache_pool seqlock hit | 30 ns | seqlock load + memcpy 1024B (L1 cache absorbed when single-thread) |
| R2miss | PROBE_OP("R2miss") | cache_pool MISS; decision to forward_read | 3 µs | seqlock retry + key compare |
| R3 | PROBE_OP("R3") | forward_read sent (cross-host) | 10 µs | ReadRing RT + HAZARD protect/release |
| R4 | PROBE_OP("R4") | forward_read ACK observed | 0.4 µs after R3 | included in R3 |
| R6 | PROBE_OP("R6") | return from search() | 1 µs | function epilogue + cache_pool_insert |

### Invalidate path (InvalReceiver thread on receiver host)

| Stage | Probe tag | What it measures | Expected p50 | Source |
|---|---|---|---|---|
| I1 | PROBE_OP("I1") | send_invalidate ring tail fetch_add | 1.4 µs | CXL atomic |
| I2 | PROBE_OP("I2") | flush_line + sfence after writing InvalEntry | 0.2 µs | clflushopt + sfence |
| I3 | PROBE_OP("I3") | InvalReceiver loop entry (new entry detected) | inter-op | not a cost |
| I4 | PROBE_OP("I4") | invalidate handler entry | 50 ns | function dispatch |
| I5 | PROBE_OP("I5") | cache_pool_set_stale done | 50 ns | atomic store + bucket_epoch bump |
| I6 | PROBE_OP("I6") | ACK back to producer (resp_op_id store) | 50 ns | seqlock-style ACK |
| I7 | PROBE_OP("I7") | producer observed resp_op_id | inter-op | not a cost |
| I8 | PROBE_OP("I8") | producer cleared req slot | 50 ns | atomic store |

### iter-12A Phase 5 RCA micro-stages (sub-decomp inside forward_write/forward_read; use only when investigating P5 path)

| Stage | What it measures |
|---|---|
| P5W_FA | TSC at entry to `forward_write_direct` (before fetch_add) |
| P5W_SR | TSC after fetch_add(tail) + flush — slot index acquired |
| P5W_SF | TSC after slot-free wait completes |
| P5W_PP | TSC after publishing req fields + flush |
| P5W_PT | TSC at start of spin-wait for resp |
| P5W_OK | TSC when resp_op_id == expected (success) |
| P5W_TO | TSC at timeout decision (rare) |
| P5R_PL | TSC at start of cache_pool_lookup |
| P5R_AK | TSC at register-fill ACK received |
| P5R_VS | TSC at value bytes loaded into out buffer |
| P5R_GH | hazard_protect entered |
| P5R_GX | hazard_release exited |
| P5R_GZ | pool->read direct LD-CXL completed (HAZARD path) |

These sub-stages allow finer-grained P4.3 analysis when a coarser
stage (W10, R3) shows anomaly. Use cumulatively, not in addition.

---

## B. Stage spec re-check (per C8 process gate)

Before EVERY path_decomp run in P4:

1. `grep -oE 'PROBE_OP\("[A-Z][A-Z0-9a-z_]*"' src/cxl_kv_ops_A.cc | sort -u`
   → compare against the stage list in §A above
2. If mismatch (new tag introduced, old tag deleted): **auto re-stage**
   - update this spec inline
   - commit `[iter14A-restage] <reason>`
   - append entry to `iter14A_progress.md` "Auto-notify events"
   - **do NOT block on user OK**; continue path_decomp

---

## C. Cross-stage anomaly triggers (P4.3 fix-loop)

For each top-level stage S in §A:
- Healthy if observed p50 < expected p50 × 2 (H/E < 2)
- Soft anomaly if expected × 2 ≤ H < expected × 5
- Hard anomaly if H ≥ expected × 5 (or > 5 µs absolute, whichever is
  lower)

Fix-loop trigger:
- Soft anomaly with clear cause → P4.3 fix attempt
- Hard anomaly → must investigate before P4 phase exit

---

## D. Cell selection for P4 canonical path_decomp

| Cell | What it surfaces |
|---|---|
| workload-a T=64 cache=on KV=1024 | high contention (Zipf θ=0.99 hot key) on cache_pool + dir lock |
| workload-a T=4 cache=on KV=1024 | low concurrency baseline |
| workload-c T=64 cache=on KV=1024 | pure-read steady state; should be all R0/R2hit |
| workload-b T=64 cache=on KV=256 | mostly local cache hits (95% read 5% update Zipf cache=on) |

Each cell: 5 reps with FUSEE_PROBE=1, FUSEE_PROBE_DUMP set; 12-rep
anomaly retry per spec §3 if needed.

---

## E. Little's Law sanity check

For each canonical cell, compute:
- Measured throughput X = trans_agg_thpt
- Average op latency L = sum of stage p50s (or w_avg_ns + r_avg_ns
  weighted by op mix)
- Theoretical upper bound: T / L (T = num threads)
- Gap = (T/L - X) / (T/L)

Gap > 50% → critical path has off-stage contention (lock queueing,
scheduling, GC) not captured by current PROBE_OP. Trigger P4.3
investigation.
