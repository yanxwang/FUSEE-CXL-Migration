# iter-8A Phase 4 RAP — ForwardRing timeout 200 ms → 5 ms

**Date**: 2026-05-04
**Inputs**: Phase 1.A µbench baseline + Phase 2 collapsed-cell probe data + Phase 3 attribution

## Phase 3 attribution (one line)

In probe data of force-collapsed `workload-d kv=1024 T=64 cache=on`
(0.0054 Mops/s, trans_wall 36 sec):

| Stage transition | Samples | p50 | p99 | max |
|---|---|---|---|---|
| **R3→R1** (cross-host CACHE_REGISTER + return + next op) | 133 | **200.0 ms** | 200.0 ms | 400.0 ms |
| W12→R1 | 2460 | 0.5 µs | 1.4 µs (with rare 200 ms outliers) | 200 ms |

**50% of cross-host CACHE_REGISTERs fire the 200 ms timeout** in
`forward_spin_wait`. Per worker's per-iteration pause = 200 ms × N
cross-host misses. Trans wall = 36 sec is consistent with ~180
hits/worker × 200 ms = 36 sec.

## Why iter-6A's fix didn't catch this

iter-6A Phase 6 reduced **InvalRing** timeout 200 ms → 5 ms (in
`send_invalidate`). The **ForwardRing** path (used by
`forward_to_owner` for cross-host writes AND `forward_cache_register`
for cross-host reads) still uses **200 ms** in
`forward_spin_wait` helper (`src/cxl_kv_ops_A.cc:47`).

iter-7A diagnosis missed this because Phase 3 probed only HEALTHY
runs (Heisenberg defeat). iter-8A defeated Heisenberg (try 2 of 8
captured a collapse) → probe data exposes the timeout firing.

## STATE (the proposal)

Reduce `forward_spin_wait` timeout from 200 ms to 5 ms. ~3 LOC change:

```cpp
// src/cxl_kv_ops_A.cc:47
const uint64_t kBudgetUs = 5000;  // was 200000 (200 ms)
```

## ATTACK VECTORS (≥6 / 6 categories)

1. **PERFORMANCE**: at workload-d cell where 50% of CACHE_REGISTERs
   timeout, this drops the per-timeout cost from 200 ms to 5 ms —
   40× reduction. Worker time-per-op cap drops to 5 ms, so
   trans_wall_max upper bound drops 40× as well. Estimated cell
   thpt 0.005 → 0.2-1 Mops/s minimum (still bounded by other
   factors, but no longer wall-clock-stuck).

2. **CORRECTNESS**: SAME semantics as iter-6A's InvalRing fix.
   When timeout fires, `forward_to_owner` / `forward_cache_register`
   returns -11 (silently). Caller (worker) propagates this
   to the runner which counts the op as failed. **§I9 strict-A is
   weakened identically to invalidate path** — both already had
   silent -11 absorption (iter-7A backlog item: fail-loud propagation).
   No NEW correctness regression.

3. **GENERALITY**: works for any workload, any T, any KV size.
   ForwardRing roundtrip in healthy case is ~10 µs (forward owner
   side runs full execute_write_local — heavier than cache_register
   alone). 5 ms cap is 500× the typical, plenty of headroom.

4. **COMPLEXITY**: 1 constant change. Mirrors iter-6A's pattern
   exactly. No new abstractions.

5. **PRIOR ART**: iter-6A InvalRing 200ms→5ms (commit `5e4a773`).
   Same proven lever applied to sister channel.

6. **IMPLEMENTATION FEASIBILITY**: instant. Single file edit.

## ABLATION CHECK

- A-1: drop ForwardRing timeout to 1 ms. **REJECTED**: forward
  responder runs full `execute_write_local` which can take 30 µs
  p99 — at 1 ms cap, healthy ops would intermittently false-timeout.
  5 ms = ~150× p99 = safe.
- A-2: keep 200 ms but add fail-loud propagation. **REJECTED for
  this iter**: doesn't reduce wall-clock damage. Should ALSO ship
  fail-loud (per iter-7A backlog QR8) but it's a separate fix; QR4
  of iter-7A reused → ship ONE fix per iter.
- A-3: K-shard ForwardResponder so saturation doesn't pile up.
  **DEFERRED**: architectural change > 200 LOC. Phase 4 named root
  cause as "timeout cap too high", not "responder is saturated";
  attribution table doesn't show responder CPU saturation
  evidence. iter-9A if Phase 6 verification shows 5 ms cap
  insufficient.

## PRIOR ART CHECK

iter-6A's RAP for InvalRing cacheline split + 5 ms timeout:
- Cacheline split is the spec-clean fix for cross-host SPSC
- 5 ms timeout is the bug-floor mitigation (not the spec-clean fix)
- iter-6A explicitly noted: "5ms cap is bounded mitigation; root
  cause investigation deferred to iter-7A"

iter-8A applies the **timeout cap** to ForwardRing without doing the
cacheline split — because Phase 1.A µbench shows ForwardEntry's
ping-pong cost is bounded (~ few µs), and Phase 3 attribution
doesn't surface it as a hot stage. The 200 ms timeout cap is the
direct cause of the collapse symptom.

## VERDICT: ACCEPT

3-LOC change. Mirrors proven iter-6A pattern. Bounded mitigation
that takes the cell wall from 36 sec → ≤ 5 ms × N timeouts.

## DECISION

Change `kBudgetUs` in `forward_spin_wait` (`src/cxl_kv_ops_A.cc:47`)
from `200000` (200 ms) to `5000` (5 ms). Document in commit.

iter-9A backlog (NOT in this iter):
1. Fail-loud on send_invalidate / forward_to_owner / forward_cache_register
   returning -11 (currently silently absorbed → §I9 weakened under
   adversarial timing)
2. ForwardEntry cacheline split mirroring iter-6A InvalRing
3. K-shard ForwardResponder (only if Phase 6 verification shows 5 ms
   cap insufficient)
