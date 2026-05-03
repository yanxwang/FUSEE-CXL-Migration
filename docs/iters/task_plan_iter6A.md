# Task plan — iter-6A — Protocol A: lock down the T≤32 throughput collapse, then optimize

**Author**: Claude
**Date drafted**: 2026-05-02 (post iter-5A summary 11:00 CDT)
**Status**: DRAFT — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter6A-tgrid]`, `[iter6A-probe]`, `[iter6A-measure]`,
`[iter6A-rap]`, `[iter6A-fix]`, `[iter6A-sweep]`, `[iter6A-misc]`)

**Spec**: `docs/design_goals.md §Protocol A (§I-XIII)`,
`docs/scaling_ycsb_spec.md` (T grid updated 2026-05-02 — drop T=86)
**Predecessor**: `docs/iters/iter5A_summary_20260502.md`

---

## TL;DR

iter-5A delivered §I9 strict-A (G6 violations=0) + KV size dim, but
exposed **two unsolved performance puzzles** and **one unmeasured
claim**:

- workload-a peak: 17.90 Mops/s @ T=64 KV=256 (89.5% of 20 Mops/s).
  But: T=1..32 collapse to 0.0007 - 0.245 Mops/s; T=86 collapse to
  0.246. Bi-modal "T=64 一枝独秀" — cannot be a clean function of T.
- iter-5A summary claimed "invalidate roundtrip ~50-200 ms p99" but
  this number was never directly measured; theoretical floor is
  ~5-10 µs (forward ring measured this in iter-4A). 4-5 orders of
  magnitude gap means the claim is suspicious — likely the
  spin_wait timeout (200ms by design) is firing on lost ACKs.

**iter-6A primary goal**: per user instruction 2026-05-02 —
**lock down where the bottleneck IS, with the finest-grained
timestamp instrumentation, BEFORE proposing any optimization.**

If a phase wants to claim "the bottleneck is X" it MUST have:
- Per-stage µs timestamp data
- p50 / p99 / max / std-dev numbers
- Counter for "X happened N times in Y wall-time"

No optimization phase ships until the bottleneck is named with
direct measurement.

**iter-6A user-visible success criterion (added per user
instruction 2026-05-02)**: workload-a throughput must show
**reasonable linear scaling** across T ∈ {1, 2, 4, 8, 16, 32, 64}
— each pre-saturation doubling of T yields ≥ 1.5× throughput, no
regression after saturation. The bi-modal "T=64 stands alone,
others collapse" shape iter-5A produced is explicitly forbidden.
Concrete table in Phase 6 §3. Even if absolute peak doesn't reach
the 20 Mops/s target this iter, fixing the shape is the gating
deliverable — you cannot optimize a bi-modal curve, you must fix
its shape first.

---

## Decision recap (incorporated into this plan)

User instructed 2026-05-02:

1. **Spec T grid changes**: `THREADS="1 2 4 8 16 32 64"` (drop 86).
   Rationale: 22 cores reserved for system threads (forward
   responder, inval dispatcher, future K-shard) + their own
   multi-thread scaling experiments. Already committed to spec.
   Total cells: 210 (was 240).
2. **Diagnostic-first methodology**: take workload-a as the reference
   workload. Lock down which stage is the bottleneck for T=1, 2, 4,
   8, 16, 32 (the collapsed cells) via finest timestamp probes on
   EVERY stage of the write-path. Only after bottleneck is named
   propose an optimization.

---

## Plan overview

8 phases. Phases 1-4 are pure diagnostic (no functional code
changes); Phase 5 is the RAP for the diagnosed-cause fix; Phase 6
implements; Phase 7 verifies + sweeps; Phase 8 cleanup.

**Hard constraint** (CLAUDE.md + per-user instruction): no Phase 5
"propose optimization" until Phase 4 has named the bottleneck with
direct measurement. Optimization without measurement = guessing.

---

## Phase 1: Spec T grid + sweep script (already partially done)

**Goal**: bring spec, sweep script, plot script, and runner into
agreement on the new T grid.

**Spec coverage**: spec §3 + §5 + §11 + §13.

**Code changes** (status):
- ✅ `docs/scaling_ycsb_spec.md`: T grid `1 2 4 8 16 32 64`; total
  cells 210; wallclock 50-80 min; iter-completion gate threshold
  210 (already committed in earlier block this session).
- MODIFIED `scripts/run_iter5A_sweep.sh` → copy to
  `scripts/run_iter6A_sweep.sh`: drop T=86 from THREADS list.
- MODIFIED `docs/tools/plot_iter4A_redo.py` (or copy to
  `plot_iter6A.py`): drop T=86 from `T_AXIS`; verify x-axis labels.

**Validation experiment**:
1. Sweep script smoke: 1 cell only (workloada T=2 KV=256), confirm
   SUMMARY.log line appears.
2. Plot script smoke: feed iter-5A's SUMMARY.log to new plotter,
   confirm 25 PNGs render with x-axis ending at T=64 (the T=86
   data is silently dropped).

**Success criterion**: spec + script + plotter consistent; one
end-to-end smoke produces a SUMMARY line + plot.

---

## Phase 2: Probe infrastructure — finest timestamp on every stage

**Goal**: instrument every stage of the write-path AND the
invalidate path with µs-precision timestamps, so Phase 3 can attribute
wall-clock latency per op to specific stages.

**Spec coverage**: instrumentation only — no I/AP touched.

**Stages to instrument** (write path):

| # | Stage | Probe location |
|---|---|---|
| W1 | Worker enters `execute_write_local` | top of fn |
| W2 | Acquired directory spinlock | after `slot_directory_lock(de)` |
| W3 | bitmap scan complete; about to broadcast | end of bitmap loop |
| W4 | First `send_invalidate` enqueue starts | inside loop, before producer cookie |
| W5 | First `send_invalidate` returns (ACK seen) | after spin_wait |
| W6 | Last `send_invalidate` returns | after broadcast loop |
| W7 | Pool.alloc returned blk_off | after pool_->alloc() |
| W8 | Pool.write returned (value flushed) | after pool_->write() |
| W9 | Slot publish flushed | after publish_slot_cow |
| W10 | Directory state stored | after de->state = ... |
| W11 | Cache pool inserted | after cache_pool_insert |
| W12 | execute_write_local returns | end of fn |

**Invalidate path** (separate thread context):

| # | Stage | Probe location |
|---|---|---|
| I1 | Producer reserves InvalRing tail (fetch_add) | after `tail.fetch_add` in `send_invalidate` |
| I2 | Producer wrote entry + flushed | after `flush_line(e); store_fence` |
| I3 | Dispatcher saw new tail (CXL coherent load returns >head) | inside `cache_dispatcher_loop` after tail load shows progress |
| I4 | Dispatcher loaded entry req_op_id | after entry flush+load |
| I5 | Dispatcher called cache_pool_set_stale | before & after stale set |
| I6 | Dispatcher stored resp_op_id + flushed | after ack flush+sfence |
| I7 | Producer poll observed resp_op_id == op_id | inside spin_wait when match |
| I8 | Producer consumed slot (req_op_id=0) | after slot free |

**Forward path** (cross-host write, separate thread context — note
this calls execute_write_local on owner side, so W1-W12 will fire
in responder context too):

| # | Stage | Probe location |
|---|---|---|
| F1 | Worker enters `forward_to_owner` | top of fn |
| F2 | Worker reserved ForwardRing tail | after fetch_add |
| F3 | Worker wrote entry + flushed | after entry flush |
| F4 | Responder saw new tail | inside responder_loop |
| F5 | Responder dispatched (calls execute_write_local) | before dispatch |
| F6 | Responder ACK stored + flushed | after resp flush |
| F7 | Worker poll observed ACK | inside spin_wait match |

**Read path** (single-host miss + cross-host miss via OP_CACHE_REGISTER):

| # | Stage | Probe location |
|---|---|---|
| R1 | Worker enters `search` | top of fn |
| R2 | cache_pool_lookup returned (hit/miss) | after lookup |
| R3 | (if miss) forward_cache_register enter | top of fn |
| R4 | (if miss) ACK observed + value extracted | after spin_wait + flush+load value |
| R5 | (if owner-self miss) bucket scan complete | after for-loop |
| R6 | search returns | end of fn |

**Code changes**:
- NEW `src/cxl_probe.h`: `struct ProbeFrame {const char *tag; uint64_t ns;}`;
  `void probe_enter(const char *tag);` `void probe_exit(int op_id);`
  with TLS ring buffer of last 4096 frames; flush to file at exit.
- MODIFIED `src/cxl_kv_ops_A.cc`: add `PROBE("W1");` macros at all
  W/I/F/R stages above. Macro is no-op when `FUSEE_PROBE=0`
  (default), active when `FUSEE_PROBE=1`.
- NEW `tools/parse_probes.py`: read TLS ring buffer dump, compute
  per-stage histogram (p50, p99, max, mean) per (workload, T, KV)
  cell across N ops.

**Validation experiment**:
1. Build with `FUSEE_PROBE=1`. Run `protocol_a_local_test` —
   probe overhead < 5% on baseline test (instrumentation must not
   distort measurement).
2. Dump probe data on a 10-op smoke run. Verify all stages fire in
   expected order (W1 < W2 < W3 < ... < W12 within one op).

**Success criterion**: probe macros compile + run; per-stage timing
data extractable per op; overhead bounded.

**Bottleneck check**: probe overhead p99 < 200ns per probe; total
overhead per op (12 W probes × 200ns = 2.4 µs) ≤ 5% of even the
fastest expected op time.

---

## Phase 3: Single-host workload-a baseline — no invalidation in path

**Goal**: measure the W1-W12 + R1-R6 stages in the **simplest case**
where invalidation is never triggered. This establishes the
"pure CoW + cache" baseline.

**Spec coverage**: I6, I9 (single-host fast path), I10.

**Setup**: H=1 (`FUSEE_NUM_HOSTS=1`), 1 worker, workload-a 50k ops.
With H=1, sharding routes everything owner-self. Sharer_bitmap
never includes a peer host (no peer to register), so
`send_invalidate` is never called. Pure local CoW + slot publish +
cache update path.

**Code changes**: none (use Phase 2 instrumentation).

**Validation experiment**:
1. Run probe-instrumented `protocol_a_local_test` with workload-a
   50k ops; T ∈ {1, 2, 4, 8, 16, 32, 64}.
2. For each T, dump per-stage histogram. Specifically:
   - W2 - W1 = directory lock acquire latency
   - W6 - W3 = total invalidate broadcast (should be 0 — H=1)
   - W7 - W6 = pool alloc latency
   - W8 - W7 = pool write latency (value bytes to CXL)
   - W9 - W8 = slot publish (CoW commit) latency
   - W10 - W9 = directory state update
   - W11 - W10 = cache update
   - W12 - W1 = total per-op latency
   - throughput = (ops × num_workers) / total_wall
3. Plot per-stage breakdown stacked bar per T (1-7 stages × 7 T values
   = single grouped chart). Output to
   `docs/iter6A_probe_baseline/single_host_breakdown.png`.

**Success criterion**: per-stage breakdown is recorded for all 7
T values; sum-of-stages ≈ measured per-op latency (≤ 5% gap from
unaccounted overhead).

**Hypothesis to verify**:
- If single-host T=1 throughput is 5+ Mops/s and T=32 throughput
  is 50+ Mops/s, then the 2-host path's collapse is **entirely**
  in the invalidate / forward channel, not in the local CoW or
  cache machinery.
- If single-host throughput is also low at T=1-32, then the
  problem is in the local path itself (e.g., directory spinlock
  contention, pool alloc, cache update).

This is a **falsifiable test**. Single-host should be FAST. If it
isn't, the hypothesis "invalidate is the bottleneck" is wrong and
we redirect Phase 4 to local-path investigation.

---

## Phase 4: 2-host workload-a — full path probes; LOCATE bottleneck

**Goal**: with Phase 2's instrumentation, measure the full 2-host
write+read path on workload-a at T ∈ {1, 2, 4, 8, 16, 32}. Use
the per-stage breakdown to identify which single stage dominates
the wall-clock latency.

**Spec coverage**: I9 (strict-A path), I10 (commit point), I11
(cross-host forward).

**Code changes**: none (Phase 2 + Phase 3 infrastructure).

**Validation experiment**:
1. Run probe-instrumented `protocol_a_ycsb` on g3+g4: workload-a,
   T ∈ {1, 2, 4, 8, 16, 32, 64}, KV=256, cache=on, 50k ops cap.
   Per-host probe dump goes to `/tmp/probe_h<id>_T<T>.bin`,
   collected back via rsync.
2. For each T, post-process probe data:
   - Per-op write latency breakdown (W1..W12 deltas, per host)
   - Per-op read latency breakdown (R1..R6 deltas)
   - Invalidate path histogram per send_invalidate call (I1..I8)
   - Forward path histogram per cross-host write (F1..F7)
3. **Identify the bottleneck stage** by largest contribution to
   p99 wall-clock per op. Report:
   - Stage name (e.g., "I7: producer poll wait")
   - Stage p50 / p99 / max
   - % contribution to total per-op latency
   - Per-host call count (so per-host throughput is derivable)
4. Specifically look for:
   - Is I7 (producer waiting for invalidate ACK) saturating at
     200ms = the spin_wait timeout? → ACK lost / not flushed
   - Is I3-I4 gap large? → dispatcher CPU starved
   - Is W3-W6 large but I1-I8 fast? → broadcast is the cost
     (not the timeout)
   - Is dispatcher count → ack rate ratio bottlenecked? →
     parallelism issue
5. Output per-stage attribution table per T to
   `docs/iter6A_probe_2host/breakdown_T<T>.md`.

**Success criterion**: per T, the single dominant stage is named
with measurement. Sum-of-stages ≈ total per-op latency. **The
output answers "where is the time going" with concrete numbers.**

**Bottleneck check**: at T=1 the lowest-priority stages (cache
update, directory update) should be < 1µs each; if one of these
dominates, the diagnosis pivots.

**Falsifiable predictions for Phase 4**:
- If "I7 = 200ms p99 in >50% of writes" → ACK loss confirmed →
  Phase 5 RAP focuses on InvalEntry layout / flush ordering
- If "I3-I4 gap = 10ms+ at T=64" → dispatcher CPU starvation → 
  Phase 5 RAP focuses on dispatcher CPU pinning + K-shard
- If "W6-W3 = 1ms+ but I1-I8 = 5µs" → broadcast count is the cost,
  not per-invalidate latency → Phase 5 considers batched broadcast
- If single-host (Phase 3) was already slow at T=1 → invalidate
  isn't the issue; rerun Phase 4 with a different focus

---

## Phase 5: RAP — design fix targeted at Phase 4's named bottleneck

**Goal**: per spec §XIII Reviewer Attack Process, write a full
RAP for the optimization that targets the Phase 4 bottleneck.

**Spec coverage**: §XIII RAP mandatory.

**This phase has no code; it's the design doc**. Content is
deferred until Phase 4 names the bottleneck — but the RAP TEMPLATE
is fixed:

```
STATE: <fix to apply, in concrete terms>
ATTACK VECTORS:
  1. PERFORMANCE: <how much speedup expected, why>
  2. CORRECTNESS: <does it preserve §I9 strict-A, G6 hash-diff>
  3. GENERALITY: <works for any H, T, KV, workload>
  4. COMPLEXITY: <LOC delta, new abstractions>
  5. PRIOR ART: <which paper / iter pattern this echoes>
  6. IMPLEMENTATION FEASIBILITY: <dependencies, risks>
ABLATION CHECK: <list 2+ alternatives ruled out, why>
PRIOR ART CHECK: <is there a published reference disagreeing>
VERDICT: ACCEPT / MODIFY / REJECT
DECISION: <what we ship>
```

**Pre-loaded RAP candidates** (for whichever Phase 4 names):

- **C-A: K-shard cache_dispatcher** (if dispatcher saturation
  confirmed). Same lever as iter-3A K-channel forward path.
- **C-B: dispatcher CPU pinning + isolcpu** (if CPU starvation
  confirmed). pthread_setaffinity_np + add cpuset to host setup.
- **C-C: InvalEntry layout fix — separate cachelines for req/resp**
  (if false-share confirmed via I3-I4 gap pattern). Pad InvalEntry
  to 2 × 64 B with req on first line + resp on second.
- **C-D: ack lost / poll race fix** (if I7 = 200ms timeout in >50%
  of writes). Investigate: producer flush_line on resp_op_id load
  before sample? Acquire-release ordering correct?
- **C-E: batched broadcast** (if broadcast count dominates) —
  collect all peer hosts into one batch entry; dispatcher applies
  N stales per dequeue.
- **C-F: dispatcher polling tightness** (if grep finds usleep instead
  of pause). Switch to `__builtin_ia32_pause()` only.

iter-6A only ships ONE RAP-validated fix per phase 5 (the dominant
bottleneck). Other candidates queued for iter-7A unless Phase 4
shows multiple bottlenecks of comparable size — in that case Phase
5 ships 2 RAPs.

**Validation experiment**: RAP doc reviewed; specific fix selected.

**Success criterion**: chosen RAP ACCEPTed; 2+ alternatives
explicitly ruled out; decision recorded.

---

## Phase 6: Implement Phase 5's chosen fix

**Goal**: apply the Phase-5-RAP-validated fix.

**Spec coverage**: depends on chosen fix; cite I/AP at commit time.

**Code changes**: depend on chosen fix. Estimated 100-300 LOC
based on candidate templates above.

**Validation experiment**:
1. Run probe-instrumented sweep on workload-a T ∈ {1, 2, 4, 8, 16,
   32, 64}, KV=256, 50k ops. Compare per-stage breakdown vs
   Phase 4 baseline.
2. **Pass criterion (per-stage)**: the targeted stage's p99 must
   drop ≥ 5× for the fix to count as "working". Total per-op
   wall-clock must drop in proportion (not just one stage's number
   drops while another absorbs the lost time).
3. **Pass criterion (scaling shape) — added per user 2026-05-02**:
   workload-a throughput must show **monotonically non-decreasing
   scaling** across T ∈ {1, 2, 4, 8, 16, 32, 64}, with each
   doubling of T producing **at least 1.5× throughput** until
   saturation kicks in (defined as "the doubling-step where
   throughput growth first drops below 1.5×; everything before
   must satisfy 1.5×; everything after must still be
   non-decreasing"). Concretely:

   | T transition | Min ratio (pre-saturation) | Min ratio (post-saturation) |
   |---|---|---|
   | T=1→2 | ≥ 1.5× | n/a — saturation cannot start at T=2 |
   | T=2→4 | ≥ 1.5× OR mark T=2 as saturation point | ≥ 1.0× |
   | T=4→8 | ≥ 1.5× OR mark T=4 as saturation point | ≥ 1.0× |
   | T=8→16 | ≥ 1.5× OR mark T=8 as saturation point | ≥ 1.0× |
   | T=16→32 | ≥ 1.5× OR mark T=16 as saturation point | ≥ 1.0× |
   | T=32→64 | ≥ 1.5× OR mark T=32 as saturation point | ≥ 1.0× |

   The bi-modal "T=64=17.9, T=others<0.5" pattern from iter-5A is
   explicitly forbidden — that's the failure shape iter-6A must
   eliminate. The reference acceptable shape is iter-5A workload-c
   cache=on (read-only): 0.67 → 1.40 → 2.40 → 3.83 → 5.33 → 7.83
   → 11.66 (each step 1.5-2.1×; saturation never visible up to
   T=86). Workload-a after iter-6A fix should follow a similar
   shape, peak somewhere ≤ 20 Mops/s.

4. G6 rw race test: violations=0 still holds (no §I9 regression).
5. G1 hash-diff battery (if 3 tempdisabled tests re-enabled in
   parallel — see Phase 8): 25/25 PASS.

**Success criterion**: stage-targeted p99 drops ≥ 5×; total per-op
latency drops accordingly; **scaling shape passes the doubling-ratio
table above**; correctness gates G1 + G6 still pass.

**Bottleneck check**: confirm the bottleneck has ACTUALLY moved by
re-running Phase 4 probes. The new dominant stage tells us "what
to fix in iter-7A". If the scaling shape passes but absolute peak
is still below 20 Mops/s, the next-iter optimization candidate is
named (the new dominant stage); iter-6A is still ✓ done.

---

## Phase 7: Sweep + iter-completion gate (G1-G6)

**Goal**: full 210-cell single-rep sweep with the fix applied.
Verify gates per spec §13.

**Spec coverage**: §IX G1-G6, §13.

**Code changes**:
- MODIFIED `scripts/run_iter5A_sweep.sh` → `run_iter6A_sweep.sh`:
  T list 1-64; MAX_OPS=200000 (spec value, not iter-5A's 50000
  deadline-scoped reduction); cell-isolation `sleep 0.2; chmod 666`.
- Generate plots via Phase 1's `plot_iter6A.py`.

**Validation experiment**:
1. 210-cell sweep × 1 rep × 3 KV sizes × MAX_OPS=200000.
   Wallclock budget: per spec §5 estimate 3-4h (ops cap restored
   from 50k to 200k makes each cell ~4× slower than iter-5A's
   sweep). If exceeds 6h, escalate to user.
2. **Per workload, peak Mops/s reported in `gap_to_target.md`**.
3. Workload-a peak should be ≥ iter-5A's 17.9 Mops/s (no
   regression from the fix); also workload-a T ∈ {1, 2, 4, 8, 16,
   32} are no longer collapsed and follow Phase 6's
   doubling-ratio table (≥ 1.5× per pre-saturation doubling,
   monotonically non-decreasing post-saturation). This is the
   **iter-6A primary success criterion** at the sweep level —
   iter-6A is not COMPLETE if workload-a still shows the bi-modal
   pattern (T=64 dominant, others collapsed).
4. G1 (hash-diff): re-enabled tests pass (Phase 8).
5. G2 (multi-rep): not enforced (single-rep default).
6. G3 (AP13 trip-wire): SIGABRT verified.
7. G4 (directory hit rate): probe-derived; report per (workload, T).
8. G5 (forward fraction): ~50/50 sharding.
9. G6 (rw race): violations=0 verified.

**Success criterion**: 210/210 cells valid (or FAILs documented);
gates G1+G3+G4+G5+G6 PASS; iter summary cites the timestamped dir.

---

## Phase 8: cleanups + memory updates

**Goal**: opportunistic fixes that don't fit elsewhere.

**Spec coverage**: §VII (AP16 hook), §X (H2/H4 enforcement).

**Code changes**:
- Re-enable 3 hash-diff tests (`protocol_a_2host_test`,
  `protocol_a_xhost_read_test`, `protocol_a_xhost_write_test`);
  thread `pool` + `InvalRingMatrix` params through their attach
  calls. Run hash-diff battery.
- AP16 commit-msg hook regex (deferred from iter-5A Phase 6) —
  extend `scripts/git-hooks/commit-msg` to require `AP16` citation
  when diff introduces `std::atomic` field on CXL-resident struct.
- `CxlKvBlockPool::HostCursor::bump.fetch_add`: add
  `flush_line + store_fence` after RMW. Defensive (currently no
  cross-host read).
- BucketLockTable allocation: drop from libfusee_cxl region (saves
  ~2.5 GB; v2 path doesn't use it).
- Memory entries: update `feedback_cxl_atomic_flush.md` with
  iter-6A's bottleneck findings if root cause turned out to be
  flush-ordering related.

**Validation experiment**:
1. 3 hash-diff tests build + 25/25 PASS.
2. Pre-commit hook test: intentional violation rejected.
3. BucketLockTable removal: existing tests still PASS (no implicit
   dependency in v2 path).

**Success criterion**: 3 tests re-enabled; hook + cleanup landed.

---

## Cross-phase verification matrix

| Phase | Validates I/AP/G | Cumulative LOC delta |
|-------|------------------|----------------------|
| 1 | spec § grid update | ~30 |
| 2 | probe infrastructure | ~250 (probe.h + macros + parse script) |
| 3 | single-host baseline; falsifies "invalidate is sole bottleneck" | 0 (use Phase 2 only) |
| 4 | locates bottleneck stage with measurement | 0 (use Phase 2 only) |
| 5 | RAP doc only | 0 |
| 6 | depends on chosen fix; G6 + G1 still pass | 100-300 |
| 7 | G1+G3+G4+G5+G6; spec §13 | ~50 (sweep script + plots) |
| 8 | §VII AP16 hook + cleanup | ~150 |

---

## Phase exit criteria (apply to every phase)

Same as iter-5A Phase exit criteria (CLAUDE.md +
`docs/design_goals.md §X`). Each phase ends only when:

1. ✅ Phase validation experiments PASS on g3+g4
2. ✅ Bottleneck check produces in-budget number
3. ✅ Commit message references I/AP/G covered
4. ✅ No regression on prior phase validation
5. ✅ Spec drift audit (P2) finds no orphan implementation

**iter-6A specific addition (for Phase 5/6)**: Phase 5 cannot
ship until Phase 4 names the bottleneck WITH MEASUREMENT. Phase 6
cannot ship until Phase 5 RAP is approved. Phase 6 result must
show stage p99 drops ≥ 5× before declaring success.

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Phase 2 probe overhead distorts measurement | Med | Phase 2 §2 explicitly tests overhead < 5%; if higher, switch to RDTSC instead of clock_gettime; pad TLS frames to cacheline |
| Phase 3 single-host baseline ALSO slow at T=1 → invalidate isn't the issue | Low-Med | Phase 4 falsifiability built in; redirect to local-path investigation if it fires |
| Phase 4 finds 2+ comparable bottlenecks | Med | Phase 5 ships 2 RAPs; both fixes implemented in Phase 6 with separate commits + separate measurement |
| Phase 4 finds NO single dominant stage (latency uniformly distributed) | Low | The 130× regression has to land somewhere; if Phase 4 shows uniform distribution, the probe granularity is wrong (re-add finer probes within suspect stages) |
| Phase 6 fix moves bottleneck without reducing total latency | Med | Phase 6 §2 success criterion explicitly requires total latency to drop, not just one stage; if violated, Phase 5 RAP gets revisited |
| Phase 6 fix passes per-stage p99 ≥ 5× criterion BUT scaling shape still bi-modal | Med-High | Phase 6 §3 doubling-ratio table is now an INDEPENDENT gate — fix can be "successful per stage" yet still fail iter-6A if workload-a T=1..32 still collapse. In that case Phase 5 RAP revisited or a SECOND fix added before sweep. Per-stage win without scaling-shape win = 0 user-visible gain. |
| Phase 7 sweep at MAX_OPS=200000 exceeds 6h | Med-High | iter-5A sweep at MAX_OPS=50000 was 1.7h; 4× ops → 6.8h estimated. If exceeds, escalate user (drop to 100000 vs 200000 explicitly approved by user, or split into 2 sub-sweeps) |
| Phase 8 BucketLockTable removal breaks an unnoticed dependency | Low | Build + run all unit tests; revert if regression |
| Phase 4 finds the bug is in iter-5A's `protocol_a_rw_race_test` (G6 violations=0 was wrong) | Low | If discovered, escalate to user — §I9 strict-A claim has to be revisited |

---

## Open questions for review

| # | Question | Default if no answer |
|---|----------|---------------------|
| QR1 | Probe TLS buffer size: 4096 frames per worker? More? | 4096; truncate older with stats kept; if Phase 4 needs deeper history bump to 16k |
| QR2 | Phase 3 single-host run on g3 alone (faster) or 2-host with FUSEE_NUM_HOSTS=1 (more apples-to-apples)? | 2-host invocation with NUM_HOSTS=1 — keeps init pathway identical, only sharding routes everything owner-self |
| QR3 | Phase 4 probe data archival: keep all `.bin` traces in `docs/iter6A_probe/`? Could be 100s of MB | Keep raw `.bin` for the canonical workload-a runs; aggregate per-stage histogram tables checked into `docs/iter6A_probe_2host/` markdown |
| QR4 | If Phase 4 reveals the fix needs spec §V revision (e.g., write commit point ordering changes), do we propose spec change or fix-without-spec? | Propose spec change via §X P3 (user-approved) BEFORE Phase 5 ships; never silently weaken §I9 |
| QR5 | Phase 7 wallclock: hard-cap MAX_OPS=200000 (spec) or accept iter-5A precedent of 50000? | Try 200000 first; if Phase-7 estimate exceeds 6h, escalate to user with concrete extrapolation |
| QR6 | Phase 5 K-shard variant (C-A): K=2, K=4, or measure-then-pick? | Measure: Phase 4 names dispatcher saturation rate; K = ceil(saturation / per-dispatcher capacity) |
| QR7 | Phase 6 success threshold "stage p99 ≥ 5×" — too aggressive? Too lax? | 5× = canonical "useful" threshold (matches iter-3A K-channel pattern); revisit only if Phase 4 shows the bottleneck is intrinsically <2× improvable |
| QR8 | If Phase 4 shows ACK lost (I7 200ms timeout), does iter-6A also need a "timeout fail-loud" change so writer doesn't silently fall through and break §I9? | YES — see also Phase 8; this is a critical correctness fix that should ride alongside the perf fix |
| QR9 | Doubling-ratio threshold: 1.5× pre-saturation is the canonical "useful" scaling threshold. Too strict? Too lax? | 1.5× is a published-paper-standard floor (any lower and the fix wouldn't be defensible as "scaling"). If Phase 4 shows the bottleneck has hard 1/T-1/(T+1) overhead structure that physically can't beat 1.3×, escalate to user before Phase 6 |
| QR10 | If iter-6A peaks at e.g. 10 Mops/s but scaling shape passes — call iter-6A done? Or hold for 20 Mops/s peak? | iter-6A done if shape passes AND peak ≥ iter-5A's 17.9 Mops/s. The 20 Mops/s absolute target is the iter-7A goal once shape is correct. Shape-correct ≥ peak-only because shape problems block all further optimization (you can't optimize a bi-modal curve, you have to fix shape first) |

---

## Quick stats

- **8 phases**; first 4 are diagnostic-only (no functional changes)
- **~700 LOC delta** across probe infra + chosen fix + cleanups
- **4 invariants/AP** (re-)validated (I6, I9, I10, AP16)
- **6 validation gates** (G1, G3-G6) reported in completion gate
- **NO optimization phase ships before bottleneck named with µs-precision measurement**
- **iter-6A user-visible exit gate**: workload-a throughput shape
  shows ≥ 1.5× per pre-saturation doubling of T; bi-modal pattern
  eliminated; peak ≥ iter-5A's 17.9 Mops/s

---

## Pending user review

Please confirm/modify:

1. **Phase ordering**: spec → probe infra → single-host baseline →
   2-host probe → RAP → fix → sweep → cleanup. Anything to reshuffle?
2. **Probe stage list (W1-W12, I1-I8, F1-F7, R1-R6)**: any missing
   stage you want covered? (e.g., flush_line latency itself per
   call?)
3. **QR1-QR8** open questions above. Defaults sensible?
4. **Risk register**: any phase-level risk you foresee that isn't
   listed?
5. **Phase 4 → Phase 5 gating**: should Phase 4 require user sign-off
   on the named bottleneck before Phase 5 RAP starts? (Default: no
   sign-off; Phase 5 RAP itself is user-reviewable.)
6. **Spec §V invariants potentially affected by the fix**: if Phase
   4 reveals a flush-ordering bug that the spec didn't anticipate,
   we update spec via P3 (user approval) before Phase 6 — agreed?

After your confirmation I will start Phase 1.
