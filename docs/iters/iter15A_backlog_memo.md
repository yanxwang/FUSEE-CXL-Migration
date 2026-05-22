# iter-15A backlog memo (from iter-14A)

**Date**: 2026-05-19 (original); REPRIORITIZED 2026-05-20 after iter-15A Phase 6 RCA campaign
**Source**: `docs/iters/iter14A_summary_20260519.md`
**Branch**: `feat/cxl-migration`

## 🆕 2026-05-20 REPRIORITIZATION (post iter-15A Phase 6)

iter-15A Phase 6 RCA campaign (7 sub-phases of perf c2c, perf stat,
latency analysis, doubling-ratio audit, uniform-vs-zipf control)
**refuted all 3 MESI hypotheses** in this backlog:

| Original Task | Original hypothesis | iter-15A finding | New priority |
|---|---|---|---|
| **Task 2** (W10 cache entry MESI) | KvCacheEntry 17-cacheline MESI ping-pong limits local_read at high cache% | Phase 6a: HITM count constant across cache% sweep (36-40K). Cache% drop is LLC pressure (shared lines grow 3.2×, working set exceeds L3). | **REFRAMED**: cache entry compaction still valuable but for **LLC pressure relief**, not MESI relief. Same fix idea, different reason. |
| **Task 4** (ring head/tail MESI) | WriteRing.tail cacheline ping-pong limits xhost throughput | Phase 6b: ring tail NOT a HITM hotspot (94% of HITM concentrated on framework cacheline, same address regardless of scenario). | **DOWNGRADED/REFRAMED**: ring tail isn't the bottleneck. Replace with new task: **multi-thread receiver** to break the single-thread serialization. |
| Implicit "MESI is the dominant cost" | (cross-cutting) | Phases 6.0 + 6a + 6b all return null for MESI causation. Real costs: software lock contention + LLC pressure + single-thread receiver. | **Reorient backlog toward software-level optimization, not coherence-level.** |

### Newly added top-priority tasks (from iter-15A Phase 6)

#### Task 4-new: Multi-thread receiver (replaces old Task 4)

- **Evidence (Phase 6b + 6c)**: xhost_* paths saturate at T=4-8 from T=64
  worker capacity (lost 8-16× scaling). Ring tail not the bottleneck (Phase
  6b). Real cause: single receiver thread per host serializes all incoming
  forwarded ops.
- **Fix candidate**: parallelize `WriteReceiver`, `ReadReceiver`,
  `write_handler`, `read_handler` across N threads with per-bucket locking
  on the data side.
- **Estimated gain**: 4-8× xhost throughput (limited by per-bucket
  contention beyond that).
- **Difficulty**: Major — touches the §I9 strict-A invariant analysis. RAP
  required.

#### Task 5: Hot-key replication / LFM lock break

- **Evidence (Phase 6.0c + 6d)**: at zipf-1.5 local_write, w_p99 = 5 ms =
  64-way queueing on LFM bucket lock. Under uniform (no hot key), local_write
  is **3× faster** (20 Mops vs 6.6 Mops). Direct measurement of lock
  contention cost.
- **Fix candidate**: detect hot keys at runtime, replicate to per-CPU value
  copies on writes (with periodic synchronization). iter-11A Phase 3a
  explored this but reverted (too expensive at the time). Re-investigate
  with current evidence in hand.
- **Estimated gain**: 2-3× local_write thpt under realistic Zipf workloads.

#### Task 6: CAS-retry counter instrumentation (small, deferred)

- **Evidence (Phase 6.0c indirect)**: latency p99/p50 ratios prove lock
  contention but don't directly count CAS retries.
- **Fix**: add `n_cache_pool_insert_cas_retry` and similar in seqlock loops.
  ~10 LOC change. Direct smoking-gun counter for future workloads.
- **Difficulty**: Trivial.

---



iter-14A delivered an "observe + fix" loop: F1 (cross-host write self-inval)
and F2 (LRU sampling) both ROLLED BACK per universal fix policy. P5 attribution
showed copy elim works at the BW level (B/op -28.9%) but throughput gain is
**bimodal-driven** — in slow mode, B/op AND throughput are identical between
STAGING and HAZARD+W1 (case B). Fast-mode reaches 17.5 Mops/s but is
intermittent. iter-15A's task list reflects these findings.

---

## Tier 0 — MUST RUN FIRST (carryover from iter-14A blocked deliverables)

### 0.A. Redo P6 v2 Layer 2 probe verification on fresh testbed

**Why first**: iter-14A's P6 v2 Layer 2 (probe-build run of 8 cells to
capture stage counts per scenario) was BLOCKED — probe build crashed
under T=64 + testbed CXL state corrupted from accumulated worker
segfaults + testbed reset denied (shared host). This blocked direct
evidence that:

- `local_read_*` scenarios trigger **R3 = 0** (owner-self path only,
  never forward_read)
- `xhost_read_*` scenarios trigger **R3 > 0** during warmup, then R2hit
  after cache_pool populates locally
- `local_write_*` triggers W1-W12 main path with P5W_FA = 0
- `xhost_write_*` triggers P5W_FA > 0 (forward_write_direct)

**Approach**:
1. Wait for testbed to be available (or PXE reboot if user authorizes)
2. Re-rsync source + traces + rebuild build-cxl-w1-probe with current
   FUSEE_PROBE_DUMP fix (`probe.{pid}.{tid}` naming required by parser)
3. Run Layer 2 = 8 cells × T=64 × FUSEE_PROBE=1 × MAX_OPS=200k
4. Apply P4.5 walkthrough format: for each scenario, dump stage count
   table + verify against expected path
5. ONLY IF Layer 2 passes the gate, run Layer 1 (144 cells throughput)
   for re-confirmation. If gate fails, redesign scenarios.

**Acceptance gate** (binary, written into the run):
- local_read uniform/zipf: R3 N = 0
- xhost_read uniform/zipf: R3 N > 0
- local_write uniform/zipf: P5W_FA N = 0
- xhost_write uniform/zipf: P5W_FA N > 0

If any of these fails, the microbench scenario design itself is broken
and must be revised before any throughput interpretation.

### 0.B. P7 full YCSB scaling sweep (210 cells)

Deferred from iter-14A (skipped to avoid further testbed pressure during
the chaotic auto-loop spawning incidents). Standard 5 wl × 7 T × 2 cache
× 3 KV × 1 rep on as-shipped `build-cxl-w1`. Output to
`docs/g34_scaling_ycsb_iter15A_*/` per spec.

---

## Tier 1 — High-priority attack on the 20 Mops/s gap

### 1. Bimodal RCA + escape mechanism (iter-14A P5 finding)

P5 showed slow-mode vs fast-mode = 10.7 Mops/s vs 17.5 Mops/s, with B/op
identical to STAGING in slow mode. P3.C confirmed bimodal persists at
iter-13A HEAD. iter-12A's "bimodal fix" did not eliminate it.

**Hypothesis** (worth testing first):
- Slow mode is a high-eviction TLS state where epoch_evictions ≫ hits.
  TLS counters exist (P8 E2) but never logged in production. Add
  `FUSEE_DUMP_TLS_COUNTERS=1` flag, run workloada T=64 × 20 reps, plot
  scatter of (eviction_rate vs trans_agg_thpt).
- If slow-mode reps cluster at high eviction → root cause is
  TLS-thrashing. Fix candidate: TLS sizing per worker × per workload.

**Effort**: research-scope first (E2 patch + 20-rep scatter); ~half day.
If hypothesis confirmed, follow with code fix (probably ½–1 day).

### 2. W10 structural fix — cache_pool_insert MESI on 1088 B entry

**P4 finding**: W10 = 3.54 µs p50 (3.5× spec). T-invariant (T=4 ≈ T=64),
confirming structural cause. The 1088 B KvCacheEntry layout = 17
cachelines × T cores cross-bouncing per insert. Same finding repeated in
iter-9A, iter-10A, iter-13A, iter-14A.

**Candidate fixes** (RAP needed):
- **C1**: split entry layout — metadata (key + lru_epoch + seq + ptr) on
  cacheline 0 (64 B), value_bytes in separately-allocated chunk pointed
  to from cacheline 0. Insert writes only 1-2 cachelines.
- **C2**: async write-behind — defer cache_pool_insert off critical
  path. Needs §I9 visibility analysis.
- **C3**: skip cache_pool_insert when host is sole writer. Relies on
  InvalRing to push to readers; §I9 analysis required.

**Expected gain**: P5 fast-mode B/op = 80 vs slow-mode 120. Fixing W10
would push slow-mode throughput closer to fast-mode (~17.5 Mops/s).
Combined with bimodal RCA could approach 20 Mops/s sustained.

**Effort**: 2–4 days including RAP + impl + measurement.

### 3. F1 redesign with sharer_bitmap retention

iter-14A F1 ROLLBACK root cause: `sharer_bitmap` is reset to `{owner}`
per write, so `bitmap_minus_self` is empty most of the time. Redesign:

- On TLS hit / cache_pool hit, record the reader's host in the bucket's
  sharer_bitmap (currently only writers/registerers do this).
- On invalidate, only target hosts in bitmap (existing behavior, but
  now bitmap reflects real sharer set).
- F1's self-invalidate can then save real invalidate roundtrips because
  there will actually BE cross-host sharers to invalidate.

**Effort**: medium (touches TLS + cache_pool hit paths + InvalRing).
**Risk**: §I9 ordering needs careful analysis.

### 4. Ring head/tail MESI ping-pong mitigation (Invalring / Readring / Writering)

**Discovered**: 2026-05-20 audit of `cxl_{inval,read,write}_ring.h` +
`cxl_kv_ops_A.cc` worker/receiver loops. Same MESI ping-pong family as
Task 2 (W10 entry layout); applies to the cross-host ring tail cacheline.

**Audit summary**:

| Mitigation | Inval | Read | Write | Notes |
|---|:-:|:-:|:-:|---|
| M1: head/tail on separate cachelines | ✅ | ✅ | ✅ | Fixed in iter-9A redo Phase 2 |
| M2a: consumer-cached tail snapshot | ⚠ poll-and-drain only | ⚠ | ⚠ | Outer loop re-flushes tail every iteration |
| M2b: producer-cached head | N/A | N/A | N/A | Producers don't read head (use `slot.req_op_id==0` for full-detect) |
| M3a: batched head publish | ✅ | ✅ | ✅ | Consumer accumulates in local `head`, writes ring->head once per drain |
| M3b: batched tail fetch_add | ❌ | ❌ | ❌ | Every op = one `fetch_add(1)`. `fetch_add(N)` batch path exists at `cxl_kv_ops_A.cc:878-973` but `#if 0` |
| M4: per-producer SPSC split | ❌ | ❌ | ❌ | N workers share one `ring[src][dst]` — true MPSC over CXL |

**Where it bites**: `WriteRing` tail cacheline experiences three
simultaneous contentions:
- (a) Intra-host MPSC: T workers `fetch_add` same `ring->tail`
- (b) Producer→consumer cross-host: every push flush_line evicts to CXL
- (c) Consumer→producer cross-host: receiver flush_line + load every poll

PROBE measured `P5W_FA → P5W_SR` = ~200 ns/op at workload-a T=64. At
12 Mops/s aggregate the tail isn't the binding stop yet — but it
becomes the next bottleneck once W10 and bimodal are addressed
(theoretical max if tail were free: 320 Mops/s; current 12 Mops/s →
plenty of headroom downstream, but ring head/tail will catch up).

**Candidate fixes** (RAP needed for any of these):

- **M3b unblock (cheapest)**: Re-enable `write_sender_drain_dst_v2_unused`
  at `cxl_kv_ops_A.cc:878-973`. Aggregator funnels N worker ops into a
  single `fetch_add(N)` → tail write frequency drops N×. Code already
  written + tested in iter-11A; needs revival + correctness re-verify.
  **Effort**: ~½ day (uncomment + RAP + 5-rep verify on workload-a).
- **M2a (cheap)**: Consumer outer loop should keep `cached_tail` and
  only re-flush+load `ring->tail` when `head == cached_tail`. ~3 lines
  per receiver (`write_receiver_loop`, `read_receiver_loop`,
  `cache_dispatcher_loop`). **Effort**: ~½ day with verification.
- **M4 per-producer SPSC (structural)**: iter-9A's original N:1:1:N
  design — N worker sub-rings on src host, 1 sender thread aggregates
  → 1 cross-host ring → 1 receiver thread → N handlers. iter-9A
  delivered only ~25% (CPU pinning + 2 thread names). Closes both
  MPSC tail contention AND keeps cross-host atomic single-writer.
  **Effort**: 3-5 days; full RAP required; cautionary precedent #3 in
  CLAUDE.md applies — must not silently descope.

**Expected gain**: not yet quantified. Need a microbench similar to
iter-15A 2-tier study using `perf c2c` HITM on `ring->tail` cacheline
to baseline, then ablate M3b / M2a / M4 separately.

**Order of attack** (recommended):
1. M2a first (smallest LOC, biggest "free" reduction in consumer-side
   cross-host pulls)
2. M3b second (revival of dead code; depends on aggregator state in
   `cxl_op_aggregator.h`)
3. M4 last (full N:1:1:N — only if M2a + M3b leave tail still hot)

---

## Tier 2 — Research scope (no code ship, just measurement)

### 4. P8 TLS research experiments (E2, E4, E6)

From `docs/iter14A_p8_tls_research/tls_evolution_review.md`:

- **E2 (TLS counter dump)**: <10 LOC. Adds `FUSEE_DUMP_TLS_COUNTERS=1`
  flag to print per-worker `hits / misses / epoch_evictions / replacements`
  at end-of-test. Informs assumptions A2 and A5. Also supports task 1
  (bimodal hypothesis).
- **E4 (TLS-vs-cache-pool ablation matrix)**: 4 builds (B0/B1/B2/B3) with
  tls_lookup and/or cache_pool_lookup bypassed. Isolates each layer's
  contribution to thpt.
- **E6 (bimodal vs eviction-rate correlation)**: workloada T=64 × 20
  reps; plot eviction_rate vs trans_agg_thpt scatter. Validates task 1
  hypothesis.

All three are research-scope, no shipped code change, but feed Tier 1
decisions.

### 5. P4 walkthrough new alerts (iter-14A P4.5 surfaced)

Each gets its own RAP if pursued:
- **R3 + P5R_AK measurement boundary discrepancy**: redo probe placement
  in `forward_read_direct` to align R3 outer measurement with sub-stages.
- **P5R_PL p99 = 13.6 ms outlier**: R3 path internally re-runs
  cache_pool_lookup; redundant given R2miss already flagged. Skip the
  inner lookup.
- **W12 max 12 ms preemption**: SCHED_FIFO worker pinning + isolcpus
  list. May significantly reduce p99 across all stages.
- **W9 spec overestimate**: update spec from 150 ns expected to 30 ns.

---

## Tier 3 — Tooling / methodology

### 6. P4 walkthrough automation

The walkthrough doc was hand-authored from parsed_*.tsv data in iter-14A.
For iter-15A onward, a script should auto-generate the walkthrough
skeleton (observed numbers + alerts + spec-vs-observed deltas) so the
human-authored part is just the "reasonable? + code logic + opt room"
analysis fields.

### 7. SCHED_FIFO + isolcpus on testbed

The `[A:thread] ... pinned cpu=X` lines show workers + senders/receivers
are pinned. But not SCHED_FIFO. W12 max=12 ms is OS preemption.
SCHED_FIFO + isolcpus would eliminate this entire class of jitter.
~30 min one-time setup; permanent benefit.

---

## Tier-0 process safeguards (LEARNED FROM iter-14A FAILURE)

iter-14A had 3+ hours lost to **rogue auto-loop launcher scripts** that
kept respawning P6 retry + P7 sweep + P6.5 in /tmp, repeatedly competing
for the testbed and crashing it. Discipline for iter-15A:

1. **NEVER spawn `nohup ... &` launcher chains from inside autonomous loop
   iterations**. Each loop iteration must be self-contained.
2. **/tmp scripts must be cleaned at iter end** (or at deliberate
   "pause" points). Add an end-of-iter cleanup step that:
   - `rm -f /tmp/iter*_launcher.sh /tmp/iter*_outdir.txt`
   - Verifies no `bash` processes match the iter prefix
3. **ScheduleWakeup with `<<autonomous-loop-dynamic>>` is dangerous**
   when there are mid-iter user interruptions. The user's "I'll stop
   interacting" should NOT trigger autonomous restarts of background
   sweeps. iter-15A: if Tier-0 0.A or 0.B blocks, STOP and write a
   "blocked" status — don't auto-retry via background loops.
4. **Test-bed pre-flight check on every script** that launches workloads:
   `ssh g3 'ps -ef | grep protocol_a_ycsb | grep -v grep | wc -l'` must
   return 0 before starting any new cells. If nonzero, FAIL FAST with
   error → don't proceed.

These belong in iter-15A's task plan QR (quality review) section.
