# iter-15A backlog memo (from iter-14A)

**Date**: 2026-05-19
**Source**: `docs/iters/iter14A_summary_20260519.md`
**Branch**: `feat/cxl-migration`

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
