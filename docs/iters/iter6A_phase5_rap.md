# iter-6A Phase 5 RAP — InvalEntry req/resp cacheline separation

**Date**: 2026-05-03
**Phase**: 5 (RAP per spec §XIII)
**Inputs**: Phase 4 probe data on workload-a T=2/T=8 KV=256 H=2

## Phase 4 bottleneck attribution (Phase 4 outputs feeding Phase 5)

Probe data (workload-a T=2 H=2, 50k ops):
- **I1->I2 producer write entry: 1.6 µs typical (p99 2 µs)** — fast
- **I1->I7 full producer roundtrip: 5.5 µs p50; 191 µs p90; tail extends to 200 ms timeout**
  → tail latency is the bug, not typical case

w_p99 from sweep = 198 ms = the spin_wait timeout (200 ms) firing.
Some fraction of writes hit the timeout, blowing up p99.

**Hypothesized root cause**: InvalEntry packs req_op_id (producer-write,
consumer-read) AND resp_op_id (consumer-write, producer-read) on the
SAME 64 B cacheline. Producer-consumer ping-pong forces the line
through CXL on every transition. On rare timing windows the
producer's flush+load sequence (intended to refetch from CXL) misses
the consumer's just-flushed ACK — likely because:

1. clflushopt followed by mfence then load may, on CXL Type 3 (no
   cross-host coherence), still return a STALE cache copy if the
   producer's flush hasn't fully completed before the load issues.
2. False sharing: producer's repeated writes to req_op_id (via "wait
   for slot free" loop probing) keep the line in producer's cache as
   modified — consumer's writes to resp_op_id force ping-pong.

## STATE (the proposal)

Split `InvalEntry` from 64 B (1 cacheline) → 128 B (2 cachelines):
- **First 64 B (producer-owned)**: `req_op_id`, `key`, padding.
- **Second 64 B (consumer-owned)**: `resp_op_id`, `status`, padding.

Producer never writes the second line. Consumer never writes the
first line. No producer-consumer cacheline ping-pong. Each side does
its CXL flush on its own line; the load on the OTHER's line gets a
stable refetch from CXL.

Concrete diff in `src/cxl_inval_ring.h`:
```cpp
struct alignas(64) InvalEntry {
  // Producer line (host that calls send_invalidate writes here):
  std::atomic<uint64_t> req_op_id;
  uint64_t key;
  uint8_t  _pad_p[64 - 8 - 8];
  // Consumer line (cache_dispatcher_loop writes here):
  std::atomic<uint64_t> resp_op_id;
  int32_t  status;
  uint8_t  _pad_c[64 - 8 - 4];
};
static_assert(sizeof(InvalEntry) == 128, "InvalEntry must be 2 cachelines");
```

Ring depth 256 → entry array doubles from 16 KB to 32 KB. Negligible.

## ATTACK VECTORS (≥ 6 / 6 categories)

1. **PERFORMANCE**: Cacheline-separated producer/consumer is the
   canonical SPSC pattern (see Lamport's circular buffer + Disruptor).
   Expected: tail latency 200 ms → ~10 µs (no ping-pong, no missed
   refetch). p99 dropped 4 orders of magnitude. Median I1->I7 stays
   ~5 µs (already fast). Throughput at T=2 H=2 should jump from
   0.004 Mops/s → at least 0.5 Mops/s (matching H=1 baseline).

2. **CORRECTNESS**: §I9 strict-A linearizability preserved — no
   change in protocol, only entry layout. G6 rw race test should
   still report violations=0. The semantics of "writer waits for
   ACK before proceeding" remains unchanged; the entry layout is
   transparent to the protocol.

3. **GENERALITY**: Works for any H, T, KV. Same lever applies if
   later we add K-shard dispatchers — each dispatcher's slot pair
   stays cacheline-separated. ForwardEntry has the same problem
   (req_op_id + resp_op_id in same line); same fix should apply
   there in iter-7A.

4. **COMPLEXITY**: ~20 LOC delta in `cxl_inval_ring.h` (struct
   layout); 0 LOC delta in callers (field names unchanged). Minimal.

5. **PRIOR ART**: Disruptor (LMAX, 2010); Lamport's "Specifying
   Concurrent Program Modules" — cacheline-separated SPSC. Used
   verbatim in iter-3A `PerHostSpscRing` (head + tail on separate
   cachelines, in fact `_pad_tail[64-...]` is exactly this pattern).
   ForwardEntry already has 2-cacheline form (one for req_op_id +
   payload, one for resp_op_id + status); InvalEntry was packed in 1
   line for compactness, which we now reverse.

6. **IMPLEMENTATION FEASIBILITY**: Single struct change. No new
   dependencies. Build + smoke + sweep on g3+g4. ~30 min total.

## ABLATION CHECK (alternatives ruled out)

- **A-1: Drop the 200 ms timeout, spin forever**. Verdict:
  REJECTED. Doesn't fix the underlying ping-pong; just hides
  symptoms in throughput numbers. Hides bugs.
- **A-2: Switch flush_line + mfence to clwb (write-back instead of
  invalidate)**. Verdict: REJECTED. clwb writes back without
  invalidating the line — the line stays in producer cache as
  shared, but CONSUMER side still needs to invalidate. Doesn't help.
  Also clwb is supported but slower than clflushopt on most chips.
- **A-3: K-shard dispatcher (the iter-5A leftover candidate)**.
  Verdict: DEFER to iter-7A. K-shard helps when dispatcher is the
  bottleneck (CPU-saturated). Phase 4 measurement shows dispatcher
  consumes invals in ~5 µs typical — far from saturated.
  K-sharding doesn't fix the per-roundtrip 200 ms tail.
- **A-4: Make invalidate fire-and-forget (drop ACK wait)**. Verdict:
  REJECTED. Violates §I9 strict-A, breaks G6.

## PRIOR ART CHECK (any reference disagreeing?)

- LMAX Disruptor explicitly recommends padding between producer and
  consumer cursors (1 cacheline pad each side).
- Linux kernel Lock-Free FIFO (kfifo) padded `in` and `out` cursors
  on separate cachelines.
- iter-3A `PerHostSpscRing` already follows this pattern for ring
  metadata. iter-5A InvalEntry was packed against this convention.
  No prior art supports the packed form.

## VERDICT: ACCEPT

Cacheline-separated producer/consumer InvalEntry. Single ~20 LOC
diff. Fixes a known anti-pattern.

## DECISION (what we ship)

`src/cxl_inval_ring.h`: extend `InvalEntry` to 2 cachelines (128 B);
producer fields on first line, consumer fields on second. No other
code changes needed (field names unchanged).

Phase 6 success criterion: invalidate roundtrip p99 drops ≥ 5×
(from 200 ms to ≤ 40 ms; ideally to ≤ 50 µs). Throughput shape on
workload-a T=1..64 follows ≥ 1.5× per pre-saturation doubling.
G6 violations=0 still holds.

If Phase 6 measurement shows the 200 ms tail STILL fires at high
rate after this fix, the root cause is elsewhere (likely in the
spin_wait flush ordering itself), and we revisit Phase 5 with a
deeper instrumentation pass.
