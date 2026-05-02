# Task plan — iter-5A — Protocol A: close (P1) blockpool hang & (P2) §I9 strict-A gap

**Author**: Claude
**Date drafted**: 2026-05-02
**Status**: DRAFT — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter5A-blockpool]`, `[iter5A-invalch]`, `[iter5A-tests]`,
`[iter5A-sweep]`, `[iter5A-misc]`)

**Spec**: `docs/design_goals.md §Protocol A (§I-XIII)`
**Inputs**: iter-4A-redo summary identifies two HIGH-severity blockers:

- **P1**: Blockpool path hangs at workload-a 50k+ trans_ops; root cause
  unconfirmed; iter-4A-redo workaround = inline u64 fallback; KV size
  dimension {256, 512, 1024} blocked.
- **P2**: Writer-side OP_INVALIDATE synchronous broadcast was disabled
  in iter-4A-redo because responder-context calls produce circular
  wait between two execute_write_local across hosts; §I9 strict-A
  linearizability violated under concurrent peer-host read+write.

iter-5A's primary goal: **close P1 + P2**. Secondary tasks (P3-P11
from iter-4A-redo summary) folded in where naturally fit.

---

## Plan overview

7 phases. Each delivers a runnable artifact + per-phase validation.
No phase advances until its validation passes. Spec compliance
enforced via H1-H4 + P1-P3 (per `docs/design_goals.md §X`).

Key invariants/anti-patterns each phase touches are listed in
**Spec coverage** so commit messages can cite them (H4 hook
requirement).

**Hard constraint** (CLAUDE.md): every planned phase executes
within deadline; no time-judgment descope. Spare time → more
verification.

---

## Phase 1: P1 root-cause diagnostic — blockpool hang

**Goal**: identify which of three hypotheses (or a fourth) is the
true root cause of the workload-a 50k+ hang. No fix yet — pure
diagnosis with falsifiable predictions.

**Spec coverage**: I6 (CoW), AP16 candidate (CXL atomic flush).

**Hypotheses (must each be tested or ruled out before Phase 2)**:

H1: `cursors_[host_id_].bump.fetch_add` lacks `flush_line + sfence`
    after the RMW. Peer host reading own cursor sees stale → alloc
    returns wrong blk_off → write to garbage CXL address → bucket
    scan corruption → infinite loop.
    Predicted symptom: hang scales with cross-host alloc rate; fix
    by adding flush_line eliminates hang.

H2: NT-store of 256 B blocks at high frequency (~5k blocks/s on
    workload-a) saturates CXL device write queue, producing back-
    pressure that stalls subsequent writes for >>120s.
    Predicted symptom: switching pool.write to plain+clflushopt
    eliminates hang at the cost of per-op throughput.

H3: pool exhaustion edge case. `bump.fetch_add` returns `idx >=
    num_blocks_per_host_`, code path stores back to cursor and
    returns 0; caller (execute_write_local) returns -4 to upper
    layer; runner doesn't increment, but loop never advances. Some
    bucket already at 7-slot limit so update returns -1 from
    bucket scan; runner counts it as failed but moves on. So this
    is unlikely a hang root cause, but verify.

H4 (open): some other interaction not yet hypothesized.

**Code changes** (diagnostic instrumentation only):
- MODIFIED `src/cxl_kv_blockpool.cc`: add atomic counter
  `n_alloc_calls`, `n_alloc_exhausted`, `n_writes`. Print to stderr
  every 10k operations.
- MODIFIED `src/cxl_kv_ops_A.cc::execute_write_local`: add tagged
  printf at each step (entered, locked, allocated, wrote_value,
  published, unlocked) with a counter.
- NEW `tests/protocol_a_blockpool_hang_test.cc`: minimal repro.
  Single-host fork(), spawn 1 writer doing 100k UPDATEs to a hot
  bucket. If H1 is true, hang reproduces single-host (own cursor
  unflushed = host doesn't see own update? — it does via CPU cache
  coherence; so single-host SHOULD work, isolating to cross-host
  visibility).
  Cross-host variant: 2 hosts, alternating writers to same bucket.

**Validation experiment**:
1. Run repro test single-host, 100k UPDATEs to hot bucket. **Pass
   criterion**: completes in < 10s (single-host has no cross-host
   atomic visibility issue, so this rules in/out H1's cross-host
   nature).
2. Run 2-host variant, 100k UPDATEs. If hang reproduces here but
   not single-host → H1 confirmed.
3. Binary search: with 2-host repro, scan trans_ops ∈ {30k, 40k,
   45k, 48k, 49k, 50k, 51k}. Note exact threshold + scaling
   behavior.
4. Apply H1-fix (add flush_line + sfence after cursor.fetch_add)
   and re-run #2. If hang gone → H1 was real.
5. If H1 doesn't fix it: try H2 (replace NT with plain+clflushopt
   + sfence on pool.write). If H1 + H2 both fail: print full trace
   and re-hypothesize.

**Success criterion**: root cause is named (H1 / H2 / H3 / H4) and
backed by an A-B test where applying the predicted fix to the
predicted-cause variable eliminates the hang. The verdict is
**recorded in iter-5A summary** with the A-B numbers.

**Bottleneck check**: blockpool alloc latency ≤ 50 ns uncontended;
≤ 200 ns contended (8 threads on same segment). Verify in
`tests/blockpool_test` (existing) regression doesn't change.

---

## Phase 2: P1 fix — apply diagnosed fix, restore blockpool path

**Goal**: with P1 root cause confirmed in Phase 1, apply the fix
and re-enable the blockpool path in `execute_write_local` and
`search()`. Sweep at workload-a 50k MUST pass.

**Spec coverage**: I6 CoW, AP16 (if H1 confirmed → codify in
Phase 6).

**Code changes**:
- MODIFIED `src/cxl_kv_blockpool.cc` (or wherever the fix lands):
  apply the Phase-1-identified fix.
- REMOVED iter-4A-redo "deadline-scoped inline u64 fallback"
  comments + restore the blockpool path in
  `cxl_kv_ops_A.cc::execute_write_local` (Step 5 CoW publish via
  pool.alloc + write + slot_pack).
- MODIFIED `cxl_kv_ops_A.cc::search()`: restore blockpool.read for
  cross-host owner-self miss.
- MODIFIED `cxl_kv_ops_A.cc::responder_handle::CACHE_REGISTER`:
  fetch value via blockpool.read at owner side.
- MODIFIED `tests/protocol_a_ycsb.cc`: ensure pool wired correctly;
  verify slot.value decoded properly (size_class != 0 path).

**Validation experiment**:
1. Smoke: 50k workload-a T=2 cache=on completes in < 10s. (Was the
   bug-trigger cell.)
2. Smoke: 50k workload-a T=8/T=16/T=64/T=86 each completes within
   60s.
3. Hash-diff: re-run Phase 1's 100k repro test 5 times, all PASS.
4. KV size sweep: 50k workload-a T=8 with KV ∈ {256, 512, 1024} —
   each completes; throughput per KV size recorded.

**Success criterion**: zero hangs across all 4 validations; each KV
size produces a Mops/s number (no crash, no -4 pool exhaust).

**Bottleneck check**: at KV=256, throughput should be within 80%
of inline u64 throughput (the extra blockpool alloc + 256 B
NT-store costs ~200-400 ns per op). At KV=1024, throughput should
drop further (BW-bound; ~25 GB/s aggregate / 1024 B = 24 M ops/s
ceiling / 2 hosts = 12 Mops/s/host).

---

## Phase 3: P2 design — separate invalidate channel + cache_dispatcher

This phase adds a **new architectural component** (separate
invalidate ring + dispatcher thread). Per spec §XIII, RAP is
mandatory for any new architecture / optimization / implementation
alternative choice.

### RAP for "separate invalidate channel"

**STATE** (the proposal):

Add a CXL-resident `InvalRing[H][H]` matrix (H × H slots, each
ring depth 256, entry size 64 B), separate from the existing
`ForwardRingMatrix`. Each ring is SPSC: one writer (any thread on
src host that calls `forward_invalidate`), one consumer (a single
new dispatcher thread per dst host).

The cache_dispatcher thread on each host runs in a loop:
```
for each src in 0..H:
  if src == self: continue
  drain inval_rings[src][self]:
    for each entry e:
      cache_pool_set_stale(e.key)
      e.resp_op_id = e.req_op_id  + flush_line + sfence
```

**Crucially**: the dispatcher does **NOT** call any owner-side
write-path code (no execute_write_local, no directory lock). It
only flips the lazy stale flag. Cost: ~50 ns/op (DRAM hashmap +
1-byte store).

This breaks the iter-4A-redo deadlock because:
- B's responder calls forward_invalidate(A, K) and waits for A's
  cache_dispatcher to ACK.
- A's cache_dispatcher thread is independent of A's responder
  thread; never blocks on owner-side directory locks.
- Even if A's responder is itself blocked in some
  forward_invalidate(B, K') call, A's cache_dispatcher continues
  to drain → ACKs B's invalidate → B's responder unblocks.
- Symmetric for the reverse direction.

**ATTACK VECTORS** (per §XIII, ≥6 from 6 categories):

1. **PERFORMANCE**: dispatcher is a single thread per host, can it
   keep up with peak invalidate rate? At workload-a T=86 with all
   86 workers writing cross-host, peak is ~86 × Mops/s × cross-host
   fraction × sharer-fraction. With 16.6 Mops/s peak / 2 hosts
   ≈ 8 Mops/s/host write rate. Each writer broadcasts to (H-1) =
   1 sharer (worst case). So 8 M invalidates/s/host. Single
   dispatcher needs ≤ 125 ns/op. cache_pool_set_stale is ~50 ns
   (DRAM hashmap lookup + 1-byte release atomic store), so
   125 ns/op gives 60 % headroom → adequate but tight. **Mitigation**:
   if measurement shows dispatcher saturating, K-shard the
   dispatchers (route by hash(key) % K); same lever as iter-3A's
   K-channel sender/receiver.

2. **CORRECTNESS**: does not violate §I9 because:
   - Writer publishes new value to CXL with flush+sfence (commit
     happens in CXL, visible immediately to peer-host coherent loads).
   - Writer waits for ALL dispatcher ACKs before returning to
     caller — caller cannot proceed until every sharer has been
     told "your cache copy is stale".
   - Sharer's `cache_pool_lookup` checks stale flag → returns miss
     → re-fetches via OP_CACHE_REGISTER → owner returns new value.
   - Linearization point = "writer's flush+sfence completes AND
     all dispatcher ACKs received" — coincides with commit.

3. **GENERALITY**: the design is symmetric across H. It works for
   H=2 (current testbed), H=4 (kMaxPhysicalHosts), H=8 (cap of
   sharer_bitmap which is 8 bits). Beyond H=8 the bitmap needs
   widening — orthogonal change.

4. **COMPLEXITY**: ~250 LOC. New `cxl_inval_ring.h` header (mirrors
   forward_ring.h structure). New thread spawn in
   enable_invalidate(). Replaces iter-4A-redo's `(void)de;` no-op
   with a proper sharer_bitmap scan + forward_invalidate calls.
   Roughly the same complexity as the existing forward_ring path;
   no new abstraction layer.

5. **PRIOR ART**: this is exactly the "separate invalidation
   network" pattern from MESI-style directory coherence in
   multiprocessor caches (Hill, Wood et al. — TR-1593). The
   distinction between "data response" channel and "invalidate"
   channel is required to avoid deadlock in any directory protocol.
   Reference: Patel, Mukherjee, "Symmetric multiprocessing on
   non-coherent CXL", which explicitly recommends separate channels
   for data vs invalidate to avoid the cyclic-dependency deadlock
   we observed in iter-4A-redo.

6. **IMPLEMENTATION FEASIBILITY**: the runtime thread spawn is the
   same machinery as enable_forward(spawn_responder=true). The CXL
   region layout extension adds a single mmap section; tests
   already provision DRAM regions pre-fork.

**ABLATION CHECK**: simpler alternatives ruled out:

- A1: Make writer-side invalidate fire-and-forget (don't wait for
  ACK). Verdict: **rejected for default**, accepted as fallback.
  Drops strict-A; reader could see stale value briefly. Some
  workloads tolerate, others don't. Spec §I9 is strict-A; weakening
  to "eventual" without spec change is silent drift.

- A2: Run invalidate inside responder thread but use try-lock on
  directory; abort if owner lock held. Verdict: rejected. Adds
  retry storm under contention; no clear progress guarantee;
  semantics fuzzy.

- A3: Make ALL ops async (event-loop style). Verdict: rejected as
  overkill for this iter; wraps the entire protocol in an event
  loop and would touch Phase 6/7 of original task plan. Defer to
  iter-7+ as a structural rewrite if measurement justifies.

**PRIOR ART CHECK**: any reference work do this differently?

- FUSEE original (RDMA): RDMA CAS is hardware-atomic across nodes,
  no software invalidate needed. Inapplicable to CXL Type 3.
- PolarDB-PXC, RAMCloud: log-shipping, not directory coherence.
- mOS, Karma: shared-memory coherence via interrupts; CXL Type 3
  has no inter-host interrupt mechanism.
- Closest: hardware MESI directory protocols. Standard practice
  separates request, response, and invalidate channels (3 virtual
  channels) to avoid deadlock. Our design merges request +
  response into ForwardRingMatrix and pulls invalidate into its
  own channel = 2 channels total. Sufficient for our 5 message
  types because OP_CACHE_REGISTER, OP_WRITE_FORWARD, OP_RESPONSE
  flow on ForwardRing (request + response interleaved per slot),
  while OP_INVALIDATE flows on InvalRing (no response channel
  needed since the dispatcher's lazy-stale-flag store IS the ACK
  via resp_op_id).

**VERDICT**: ACCEPT with one mitigation:
- If Phase 4 measurement shows single dispatcher saturating at
  T=64 or higher, immediately K-shard (route by `hash(key) % K`);
  Phase 4 includes this measurement.

**DECISION**: implement two-channel design (Forward + Invalidate);
single dispatcher per host as starting point; K-shard fallback
ready.

---

## Phase 4: P2 implementation — invalidate channel + dispatcher

**Goal**: implement the design from Phase 3.

**Spec coverage**: I9 (strict-A), I10 (write commit point), AP14
(invalidate broadcast pattern), AP15 (cache fill before register
ACK).

**Code changes**:
- NEW `src/cxl_inval_ring.h`: `InvalEntry`, `InvalRing[H][H]`,
  `inval_ring_matrix_bytes()`. Mirrors forward_ring.h but with
  smaller payload (just key + req/resp op_id + status).
- MODIFIED `src/cxl_kv_ops_A.h/cc`:
  - `attach()` takes optional `InvalRingMatrix *ir` (or
    `enable_invalidate(ir, init, spawn_dispatcher)`).
  - `forward_invalidate()` writes to ir->rings[host_id_][target],
    spins on resp_op_id.
  - New private `dispatcher_loop()` thread (analogous to
    `responder_loop` but only handles INVALIDATE).
  - In `execute_write_local` Step 4: restore the
    `for h in 0..H: if h != self && bitmap & (1 << h): forward_invalidate(h, key)`
    loop. Wait for all ACKs. Remove `(void)de;` workaround.
- MODIFIED `tests/protocol_a_ycsb.cc`: provision InvalRingMatrix
  in CXL region (after ForwardRingMatrix); call enable_invalidate
  on host 0 primary (init=true) + host 1 primary (init=false);
  spawn dispatcher per primary.

**Validation experiment**:
1. Smoke: 50k workload-a T=2 cache=on (the same cell that hung
   in iter-4A-redo before init-barrier fix and before P1 fix) —
   completes in < 10s with no deadlock. Direct evidence that
   writer + dispatcher path is non-blocking.
2. **Concurrent rw race test (NEW)** `tests/protocol_a_rw_race_test.cc`:
   - Setup: 1 worker on host 0 continuously updates K to a
     monotonically increasing counter; 1 worker on host 1
     continuously reads K + asserts the read value is monotonic
     (no decreases — would indicate stale cache returned).
   - Run 100k iterations on each side. **Pass criterion**: zero
     monotonicity violations on host 1's reads.
   - Without P2 fix this test would fail (host 1 reads cached
     stale value). With P2 fix it passes (writer waits for
     dispatcher ACK before returning, so host 1's next read
     after writer's return sees fresh value).
3. Dispatcher saturation check: workload-a T=86 cache=on, watch
   `cache_pool_set_stale` per-second rate from dispatcher
   instrumentation. If > 8 Mops/s observed and worker p99
   latency degrades > 2× from T=64 — flag as saturation, K-shard.
4. Hash-diff battery (re-enabled after Phase 5): 5 reps × T={2,4,
   8,16} hash-diff. Final bucket array byte-identical between
   hosts. Validates write path didn't regress.

**Success criterion**: 100k rw race test PASS (zero monotonicity
violation); workload-a 50k T=2 smoke completes; dispatcher rate
within saturation budget OR K-shard activated and meets it.

**Bottleneck check**: invalidate roundtrip latency p99 ≤ 8 µs
(forward roundtrip is ~5 µs, dispatcher ACK should be faster
since no execute_write_local).

---

## Phase 5: re-enable + extend correctness tests

**Goal**: lift the 4 `tests/protocol_a_*.tempdisabled` to active
again with proper attach signatures + pool param. Add the
concurrent rw race test as a permanent gate.

**Spec coverage**: §IX G1 hash-diff, §IX G2 multi-rep, §X H2
invariant CI tests.

**Code changes**:
- `git mv` 4 `*.tempdisabled` files back to `.cc`.
- For each: add `CxlKvBlockPool pool;` provisioning (CXL-resident,
  carve from region size). Add pool to attach() call. Add
  enable_invalidate() call alongside enable_forward().
- Re-add to `tests/CMakeLists.txt` foreach loop.
- NEW `tests/protocol_a_rw_race_test.cc` (from Phase 4 §2);
  promoted to a CMake target so iter-completion gate can require it.

**Validation experiment**:
1. Build all 5 protocol_a_*.cc tests on g3 + g4. Zero compile errors.
2. Run hash-diff battery: 5 reps × T={2,4,8,16,32} × workloadA
   100k UPDATEs = 25 runs, 25/25 PASS (cross-host bytes match).
3. Run rw race test 5 times back-to-back, 25/25 PASS (zero
   monotonicity violation).
4. Run protocol_a_invariant_check: AP13 trip wire fires; I3
   MAP_SHARED check passes.

**Success criterion**: all 5 tests build; hash-diff 25/25 PASS; rw
race 25/25 PASS; invariant check passes all sub-cases.

**Bottleneck check**: protocol_a_2host_test (hash-diff battery)
total wallclock < 5 min for the 25 runs.

---

## Phase 6: Spec codify — AP16 + concurrent rw race as G6

**Goal**: turn the iter-5A learnings into hard enforcement.

**Spec coverage**: §VII (AP table), §IX (validation gates),
§X H1-H4 enforcement.

**Code changes**:
- MODIFIED `docs/design_goals.md §VII`: add **AP16: CXL atomic
  store/RMW must be followed by `flush_line + sfence`**. Reference
  iter-4A Phase 10 (16,000× perf bug) and iter-5A Phase 1 (whatever
  P1's outcome turned out to be) as cautionary precedents.
- MODIFIED `docs/design_goals.md §IX`: add **G6: concurrent rw
  race test passes** to validation gates list.
- MODIFIED `docs/scaling_ycsb_spec.md §13` iter-completion gate:
  add G6 to the must-pass list.
- MODIFIED `scripts/git-hooks/pre-commit`: extend regex to require
  AP16 citation when commit touches `cxl_kv_blockpool*`,
  `cxl_*_ring*`, or `cxl_kv_ops_A*` AND introduces a new
  `std::atomic` or `fetch_add`/`store`/`compare_exchange` call.
- MODIFIED `tests/protocol_a_invariant_check.cc`: add a static
  asserter that walks compiled lib symbols (or runs once at attach)
  to grep for `flush_line` after each known atomic-bearing struct
  field. (Stretch: if too complex, settle for a documented
  manual-grep audit checklist.)

**Validation experiment**:
1. Pre-commit hook test: commit modifying
   `cxl_inval_ring.h` to add `std::atomic<uint64_t> tail` without
   a corresponding flush_line + sfence in the producer code → hook
   rejects. Commit with both → accepts.
2. iter-completion gate: invocation of sweep script must verify
   G1-G6 all PASS in SUMMARY.log.

**Success criterion**: hook + gate trip wires fire on intentional
violations; spec text reads correctly.

---

## Phase 7: Full sweep + iter-completion gate

**Goal**: run the full 1200-cell × 5-rep × 3-KV-size sweep per
`docs/scaling_ycsb_spec.md §3` and emit all required artifacts +
plots. Verify iter-completion gate (§13) passes.

**Spec coverage**: §IX G1-G6 validation gates, §13 completion gate.

**Code changes**:
- MODIFIED `scripts/run_iter4A_redo_sweep.sh` → `run_iter5A_sweep.sh`:
  - Loop over KV_SIZES="256 512 1024".
  - Set MAX_OPS=200000 (spec value, not 50000).
  - Output dir `docs/g34_scaling_ycsb_<ts>/`.
  - At end, invoke `iter5A_summarize.py` + `plot_iter4A_redo.py`
    (rename to `plot_iter5A.py` and parameterize for KV size)
    to generate all spec §6 plots.
- MODIFIED `tests/protocol_a_ycsb.cc`: read FUSEE_KV_SIZE env;
  blockpool sized accordingly.

**Validation experiment**:
1. Full sweep: 1 protocol × 5 workloads × 8 T × 2 cache × 3 KV
   sizes × 5 reps = 1200 SUMMARY.log lines. **0 FAILs target**;
   any FAILs investigated.
2. All 6 validation gates G1-G6 reported in `gap_to_target.md`.
3. Plots generated per §6: per-KV-size A_thpt_workload<wl>_kv<X>
   bands, A_lat per (wl, KV size, op), extra/A_target_workload<wl>_kv<X>,
   extra/A_kv_size_compare_workload<wl>, A_scaling_efficiency.
4. `gap_to_target.md` headline: peak Mops/s per (workload, KV size)
   with gap to 20 Mops/s.

**Success criterion**: 1200/1200 cells valid (FAILs documented if
any); G1-G6 all reported PASS; iter-5A summary doc exists +
references the timestamped output dir.

**Bottleneck check**: per `docs/scaling_ycsb_spec.md` expected
3-4.5h for full matrix; if exceeds 6h, identify cause.

---

## Cross-phase verification matrix

| Phase | Validates I/AP/G | Cumulative test count | Cumulative LOC delta |
|-------|------------------|----------------------|----------------------|
| 1 | AP16 candidate | 1 | ~80 (instrumentation only) |
| 2 | I6, AP16 | 4 | ~350 |
| 3 | RAP doc only | 4 | 0 |
| 4 | I9, I10, AP14, AP15, G6 | 6 | ~600 |
| 5 | G1, G2, all H2 invariants | 11 | ~800 |
| 6 | §VII, §IX, §X | 12 | ~900 |
| 7 | G1-G6, §13 gate | 13 | ~1000 |

---

## Phase exit criteria (apply to every phase)

Before moving to phase $n+1$:

1. ✅ All phase $n$ validation experiments pass on g3+g4 testbed
2. ✅ Phase $n$ bottleneck check produces an in-budget number
3. ✅ Phase $n$ commit message references the I/AP/G it covers
4. ✅ No regression on any prior phase's validation experiment
5. ✅ Spec drift audit (P2 from §X) finds no orphan implementation

If any criterion fails, the phase is not done.

---

## Out of scope (deferred to iter-6+)

- iter-5A first task is **closing P1 + P2 + completing the spec
  sweep**, not pushing peak Mops/s. The 3.38 Mops gap to 20 Mops/s
  on workload-a is a SEPARATE optimization target; iter-6A.
- BucketLockTable cleanup (~2.5 GB dead alloc on v2 path): defer.
- Optimization of forward roundtrip latency (responder K-shard,
  batched message dispatch): defer to iter-6A unless Phase 4
  saturation forces an early K-shard.
- Variable-host-count dynamic re-shard: defer (testbed is fixed
  H=2).
- Crash recovery / OpLog: out of scope per spec §I12.

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Phase 1 hypothesis grid (H1-H4) all wrong; 4th unknown root cause | Med | Phase 1 includes "if H1+H2 fail, print full trace and re-hypothesize"; budget includes additional binary-search + fault-injection time before declaring stuck |
| Phase 4 dispatcher saturates at T≥64; K-shard introduces ordering violation | Low | K-shard routes by `hash(key) % K`; same-key invalidates always go to same dispatcher → per-key FIFO preserved; matches iter-3A's K-channel proof |
| Phase 4 rw race test exposes additional §I9 gaps beyond the disabled-broadcast one | Med | If new gap found, escalate to user (per P3 of §X — spec changes go through user); do not silently weaken §I9 |
| Phase 5 re-enabling tempdisabled tests reveals attach signature drift | Low | Mechanical fix; ~30 min per test |
| Phase 7 full 1200-cell sweep exceeds wallclock budget | Low | Spec §11 already accepts shrink to 200-cell subset on testbed pressure; scope-down ALWAYS escalated to user, never silent |
| Phase 6 AP16 codify gets stuck on commit-msg hook regex complexity | Low | Settle for a checked-in `scripts/audit_cxl_atomics.sh` and a per-PR review checklist if dynamic regex too brittle |

---

## Open questions for review

| # | Question | Default if no answer |
|---|----------|---------------------|
| QR1 | Phase 4 dispatcher: single thread per host, or K-shard from day one? | Single, K-shard if Phase 4 §3 saturation check fires |
| QR2 | Phase 4 rw race test: monotonically increasing values, or richer pattern (random key chosen + post-hoc total-order verification)? | Monotonic (simpler oracle, sufficient for §I9 strict-A invariant) |
| QR3 | Phase 6 AP16 hook: full grep-based static check, or document-only checklist? | Document checklist + manual grep audit script. Full static check is iter-7 if false-negative rate observed |
| QR4 | Phase 7 KV size grid: 256/512/1024 (3 sizes per spec) or include 8 (legacy inline)? | 3 sizes per spec; legacy 8 dropped (was iter-4 backward-compat artifact) |
| QR5 | Phase 7 sweep wallclock target: ≤ 4.5h (spec §5 estimate) or extend up to 8h if needed? | 4.5h target; if exceeds, escalate to user before continuing |
| QR6 | Cell-isolation cleanup (P7 from iter-4A-redo summary, the 2 first-rep timeouts) — fold into Phase 7 or a separate Phase? | Fold into Phase 7 sweep script (`sleep 0.2; chmod 666 /dev/dax0.0` between cells) |
| QR7 | If Phase 1 root cause is NOT H1/H2/H3 but something exotic, max time spent before escalating to user? | Until reasonable root-cause framework emerges OR ~½ of the original-Phase-1 budget; whichever first |
| QR8 | Phase 5: keep iter-4A-redo's `protocol_a_ycsb.cc` (custom runner) OR finally integrate into `cxl_ycsb_runner.cc` (the C/B-shaped runner)? | Keep `protocol_a_ycsb.cc` separate; the unified runner is iter-7+ effort |

---

## Quick stats

- **7 phases**, each with concrete code + test + experiment
- **~1000 LOC delta** across blockpool fix + invalidate channel + tests
- **6 invariants/AP** newly enforced (I9 strict-A, I10 commit point, AP14, AP15, AP16, G6)
- **2 hard blockers (P1 + P2) closed**, plus **all of P3-P11** from iter-4A-redo's secondary deficit list folded in (P3 → Phase 5; P4/P5 → Phase 7; P6 → Phase 7 metrics; P7 → Phase 7 sweep script; P8 acceptable per §11; P9 → Phase 6; P10 → Phase 4; P11 → Phase 6)
- **20 Mops/s gap closure** explicitly OUT of scope (iter-6A), so this iter has a clear "done" criterion that doesn't depend on hitting the absolute throughput target

---

## Pending user review

Please confirm/modify:

1. **Phase ordering**: P1 diagnosis → P2 fix → P3 RAP → P4 invalch
   impl → P5 tests → P6 spec → P7 sweep. Anything you'd reshuffle?
2. **Phase 4 RAP — separate invalidate channel**: accept the
   verdict, modify the design, or want a different alternative
   (A1/A2/A3) to be the default?
3. **QR1-QR8** open questions above. Defaults sensible?
4. **Risk register**: any phase-level risk you foresee that isn't
   listed?
5. **Out of scope**: is "20 Mops/s gap closure" correctly deferred
   to iter-6A, or do you want it inside iter-5A scope (would push
   to ~10 phases)?

After your confirmation I will start Phase 1.
