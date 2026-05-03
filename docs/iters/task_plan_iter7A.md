# Task plan — iter-7A — Protocol A: root-cause the 19 collapsed cells (sole task)

**Author**: Claude
**Date drafted**: 2026-05-03 (post-iter-6A 04:24 CDT sweep)
**Status**: DRAFT — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter7A-repro]`, `[iter7A-probe]`, `[iter7A-hypo]`, `[iter7A-fix]`,
`[iter7A-sweep]`, `[iter7A-spec]`)

**Spec**: `docs/design_goals.md §Protocol A (§I-XIII), AP16, G6`
**Predecessor**: `docs/iters/iter6A_summary_20260503.md` +
`docs/g34_scaling_ycsb_20260503_005721/` (the 19 collapsed cells)

---

## TL;DR — exactly one task

iter-6A's 210-cell sweep produced **19 cells with throughput
collapsed to 0.0003-0.06 Mops/s** vs same-(workload, KV) neighbor
cells at 5-15 Mops/s. iter-6A did NOT diagnose them — wrote off as
"5ms timeout cascade in single-rep noise" without measurement.

**iter-7A has exactly ONE goal**: find the root cause of those 19
collapsed cells, fix it, verify zero collapse on re-sweep. Codify
the "anomaly scan" step that iter-6A skipped.

**Hard scope constraint** (this iter): everything else from
iter-6A's iter-7A backlog list is **EXPLICITLY DEFERRED**:
- K-shard cache_dispatcher → iter-8A
- Root-cause rare 5ms inval timeout → may be subsumed if root cause
  of collapsed cells turns out to be the same 5ms tail
- Fail-loud send_invalidate -11 → iter-8A
- 2 hash-diff tests re-enable → iter-8A
- BucketLockTable removal → iter-8A
- workload-a peak optimization → iter-8A

The reason for the strict scope: **iter-6A's main process failure
was scope drift** — Phase 6 victory was declared on workload-a
shape alone, sweep anomalies got "single-rep noise" tag without
verification. iter-7A must NOT repeat. Single task. Don't optimize.
Don't refactor. Don't add features. **Find the bug, fix the bug,
verify, codify the missing process step, ship.**

---

## The 19 cells (table from iter-6A sweep)

cache=on cells (the gap_to_target.md table flagged these as
< 0.05 Mops/s while same-(workload,KV) neighbor cells hit 5-15
Mops/s):

| Workload | KV | T | Mops/s | Same-KV neighbors |
|----------|----|----|--------|--------------------|
| a | 256 | 16 | 0.0005 | T=8=0.83, T=32=3.60 |
| a | 1024 | 16 | 0.0005 | T=8=1.01, T=32=2.83 |
| b | 256 | 4 | 0.0004 | T=2=0.34, T=8=2.18 |
| b | 1024 | 64 | 0.0096 | T=32=12.12 |
| c | 1024 | 4 | 0.0005 | T=2=2.04, T=8=6.20 |
| c | 256 | 16 | 0.0014 | T=8=6.49, T=32=12.22 |
| c | 1024 | 16 | 0.0587 | T=8=6.20, T=32=12.22 |
| c | 512 | 64 | 0.0106 | T=32=11.92 |
| d | 1024 | 4 | 0.0003 | T=8=5.59 |
| d | 256 | 32 | 0.0010 | T=16=8.18, T=64=0.0055 |
| d | 512 | 32 | 0.0009 | T=16=8.41, T=64=18.00 |
| d | 1024 | 32 | 0.0045 | T=16=8.32, T=64=0.0055 |
| d | 256 | 64 | 0.0055 | T=32=0.0010 (also broken) |
| d | 1024 | 64 | 0.0055 | T=32=0.0045 (also broken) |
| f | 256 | 8 | 0.0003 | T=4=0.55, T=16=4.13 |
| f | 512 | 16 | 0.0007 | T=8=1.48, T=32=0.0011 (also) |
| f | 512 | 32 | 0.0011 | T=16=0.0007 (also broken) |
| ... | ... | ... | ... | ... |

(remaining cells follow same pattern — full list in
`docs/g34_scaling_ycsb_20260503_005721/gap_to_target.md`)

**Pattern observations** (need verification, not yet measured):
- No clean function of T (T=4, 8, 16, 32, 64 all represented)
- No clean function of KV (256, 512, 1024 all represented)
- workload-d concentrated (5 cells: T=32 × 3 KV + T=64 × 2 KV;
  multi-cell connected collapse — likely one root cause)
- Most cells have neighbor cells (same wl, same KV) at 5-15 Mops/s
  → cell-level not workload-level

---

## Decision recap (must be confirmed by user)

iter-7A explicit constraints (all subject to user override):

1. **Single task only**. No optimization. No backlog work.
2. **No phase budget** (per QR7 from iter-5A/iter-6A). Diagnostic
   continues until root cause named + A-B confirmed.
3. **Anomaly scan** is the new mandatory step — codified into
   spec §13 iter-completion gate so future iters can't skip it.
4. **Test 19 cells against multiple hypotheses** before declaring
   any single root cause; the same root cause must explain ALL
   (or be split into named subgroups).
5. **iter-6A's sweep data are reference** — no re-sweep until
   Phase 5 fix; saves wallclock.

---

## Plan overview

5 phases. Diagnostic-first. No code beyond probes + the chosen fix.

| # | Phase | Purpose | Code? |
|---|-------|---------|-------|
| 1 | Reproducibility check | Are the 19 cells deterministic or single-rep noise? | No |
| 2 | Probe upgrade + persistent capture | iter-6A probe ring (4096 frames) was too small; needs persistent dump for collapsed-cell diagnostics | Yes (~150 LOC) |
| 3 | Per-cell probe + hypothesis testing | Run probes on collapsed cells; classify hang location; A-B test hypotheses | No (use Phase 2 infra) |
| 4 | Apply diagnosed fix | Single fix per Phase 3's named root cause; re-verify the 19 cells | Yes (size depends on fix) |
| 5 | Generalized doubling-ratio + anomaly scan + spec codify | Re-sweep + mandatory anomaly scan; codify into spec §13 | Yes (~50 LOC scripts + spec) |

---

## Phase 1: Reproducibility — are the 19 cells deterministic?

**Goal**: confirm the 19 collapsed cells are **reproducible** (true
bugs) vs **single-rep noise** (and we can dismiss). This is the
gating question before investing diagnostic effort.

**Spec coverage**: §3 (single-rep is "indicative not steady-state"
disclaimer — verify it).

**Code changes**: none. Use existing `protocol_a_ycsb` binary +
sweep cell command pattern.

**Validation experiment**:
1. For each of the 19 cells, re-run with **REPS=5** explicitly opted
   in (per spec §3 multi-rep carve-out for "validating a stability
   fix" / "investigating noise").
2. Per cell, record: 5 throughput values, median, min, max, spread.
3. Classify each cell:
   - **Deterministic collapse**: ≥ 4 of 5 reps < 0.5 Mops/s →
     real bug; goes to Phase 3 hypothesis testing
   - **Probabilistic collapse**: 2-3 of 5 reps < 0.5 Mops/s, others
     normal → racy bug; goes to Phase 3 with "race window" focus
   - **Single-rep flake**: 1 of 5 reps low, others normal → spec
     §3 disclaimer covers; document but don't deep-dive
   - **Non-reproducible**: 0 of 5 reps low (cell now normal) →
     iter-6A's collapse was a transient (CXL device state, OS load,
     etc.); document and exclude from Phase 3
4. Record table in `docs/iter7A_phase1_repro.md`: per cell, rep
   values + classification.

**Success criterion**: every one of the 19 cells classified into
one of the 4 buckets above with backing 5-rep data.

**Bottleneck check**: 19 cells × 5 reps = 95 runs. At ~30s/cell
(MAX_OPS=200000 estimated), ~50 min total. Cell isolation between
reps via `chmod 666 /dev/dax0.0; sleep 0.2`.

**Phase 1 exit criterion**: classification table written; count of
"deterministic" + "probabilistic" cells determines iter-7A scope.
If ALL 19 turn out to be "non-reproducible / single-rep flake",
Phase 3-4 effort is correspondingly reduced (just document + close).
If ≥ 1 deterministic, full Phase 3 attack.

---

## Phase 2: Probe upgrade — persistent capture, full op history

**Goal**: iter-6A probe ring (TLS 4096 frames) was too small —
captures only last ~256 ops per worker. Collapsed cells run
~50k-200k ops; need full op history to attribute the slow ops.

**Spec coverage**: §VII AP16 (probe is observation, not protocol);
no I/AP touched.

**Code changes**:
- MODIFIED `src/cxl_probe.h`:
  - Replace TLS 4096-frame ring with **persistent mmap'd file** —
    each thread mmaps `${FUSEE_PROBE_DUMP}/probe.${pid}.${tid}`
    pre-allocated to N MB (e.g., 64 MB → 2.6M frames per thread).
    Frames append; no wraparound; no information loss.
  - On overflow (rare with 64 MB), the ring wraps with explicit
    "OVERFLOW" sentinel frame. Caller alerted in dump.
  - Frame layout unchanged (24 B: tag + ns + op_id).
- MODIFIED `scripts/parse_probes.py`: handle the new persistent
  dump format (header includes mmap size + frame count); detect
  OVERFLOW sentinel.
- NEW `scripts/probe_anomaly_scan.py`: given a probe dump dir,
  identify the slowest 1% of ops per stage AND the longest gap
  between any two consecutive frames in any one thread (this
  catches "thread stalled here for X ms"). Output: top-K
  anomalies per stage with op_id + timestamp.

**Validation experiment**:
1. Build with FUSEE_PROBE=1; run `protocol_a_local_test` with new
   probe infra. Probe overhead measured at < 5% of test runtime.
2. Dump file size: confirm 64 MB pre-allocation works on g3+g4
   (no /tmp space issues).
3. parse_probes.py + probe_anomaly_scan.py: smoke test on the
   dump → both produce output without crash.

**Success criterion**: probe persistent dump captures full history
of a 200k-op cell without overflow; anomaly scan identifies the
slowest 1% + longest gaps with op_id linkage.

**Bottleneck check**: per-probe overhead < 200 ns (mmap'd write
should be ~50 ns on hot path). Per-cell probe storage: 200k ops
× 16 stages = 3.2M frames × 24 B = 76 MB → bumped pre-allocation
to 128 MB to be safe.

---

## Phase 3: Per-cell probe + hypothesis testing

**Goal**: run probe-instrumented cell for each "deterministic" /
"probabilistic" cell from Phase 1; classify hang location; A-B
test hypotheses.

**Spec coverage**: §XIII RAP applies if a hypothesis becomes a
proposed fix (covered in Phase 4).

**Code changes**: none. Use Phase 2 infra.

**Hypotheses to test (informed by iter-6A observations + Phase 1
classification)**:

- **H1: 5ms timeout cascade**. Once dispatcher falls behind,
  multiple producers all hit 5ms timeout, return -11; the next
  generation of producers reuses ring slots in mixed state →
  cascade. Predicted symptom: `inval_timeout_count` per cell
  approaches total inval count; many ops show I1->I7 = exactly
  5ms (the cap).

- **H2: producer wait-for-slot-free deadlock at ring wraparound**.
  Producer N reserves slot K (= tail mod 256). Slot K's previous
  occupant timed out, so its req_op_id was reset to 0. But the
  consumer might still process the OLD entry, set resp_op_id to
  the OLD op_id. New producer sees stale req_op_id == 0 (passes
  wait-for-slot-free), writes new entry, polls resp_op_id, sees
  OLD op_id, never matches NEW op_id. Predicted symptom: many
  long I2->I7 gaps; consumer's I3->I6 timeline shows it processed
  more entries than producer expected.

- **H3: blockpool exhaustion silent fall-through**. At certain
  (T, KV, ops) combinations the per-host pool fills mid-cell.
  alloc returns 0; execute_write_local returns -4; runner counts
  it as "no-op" but cell wall_clock keeps running on remaining
  workers that ALSO hit -4. Predicted symptom: pool bump cursor
  reaches `num_blocks_per_host_`; large fraction of W7 probes
  show blk_off=0; final stats show many failed writes vs
  successful writes.

- **H4: cross-cell init residue**. Sweep script doesn't fully
  reset CXL state between cells. Stale ring state, stale directory
  entries, stale pool state from previous cell affects current
  cell. Predicted symptom: collapsed cells correlate with
  particular preceding cells; first cell of each (workload, KV)
  group is fine; later cells in the same group show degradation.

- **H5: dispatcher CPU starvation under high T contention**.
  At T=64 with 64 workers, plus responder + dispatcher per host
  = 130 threads on 86 cores. Dispatcher gets few CPU cycles;
  inval queue fills; producers timeout. Predicted symptom:
  collapsed cells correlate with high T (T=32, 64); dispatcher's
  I3 polling rate is low.

- **H6: workload-d-specific Insert-then-Latest pattern**.
  workload-d uses "latest" key generator (recently-inserted keys
  are read back). The Insert path goes through INSERT (no peer
  to invalidate) but the immediate Read goes through CACHE_REGISTER
  (because peer just registered as sharer for the key the writer
  wrote). High register rate → register channel saturated.
  Predicted symptom: workload-d collapsed cells (4 of them) show
  register response p99 elevated.

- **H7: "open" — found via probe data, not yet hypothesized**.

**Procedure (per cell, A-B-style per QR7)**:
1. Re-run cell with FUSEE_PROBE_DUMP enabled. Save probe data.
2. Run `probe_anomaly_scan.py` → top-100 anomalous ops per cell.
3. Read the timeline of those 100 ops + surrounding context.
4. Classify hang location: which stage took > 1 ms? Which thread?
5. Check predicted symptom of each hypothesis against data.
6. If a hypothesis matches → record + move to next cell to see
   if same hypothesis explains other cells.
7. If no hypothesis matches → add to H7 (open) for fresh
   hypothesis derivation.

**Validation experiment**:
1. Per cell, name the suspected stage / hypothesis.
2. **A-B confirmation**: apply a targeted (non-shipping) probe-
   only modification — e.g., add `inval_timeout_count` print, or
   force ring depth to 4096, or pin dispatcher CPU — re-run that
   one cell. Predicted symptom should change.
3. Once same hypothesis explains ≥ 80% of "deterministic"
   collapsed cells with A-B confirmation → root cause named.
4. Output `docs/iter7A_phase3_diagnosis.md` with: per-cell
   classification, named root cause, A-B evidence diff.

**Success criterion**: ≥ 80% of "deterministic" cells explained
by ONE root cause (or split into ≤ 2 named subgroups, each with
A-B confirmation).

**Phase 3 exit criterion**: same as iter-5A/iter-6A QR7 —
persistence wins, no time budget. Phase ends only when root
cause is named and A-B confirmed. The remaining 20% may have
their own subgroup; iter-8A can split them off.

---

## Phase 4: Apply Phase-3-diagnosed fix

**Goal**: ship the smallest fix that addresses Phase 3's named
root cause; verify the 19 cells (or the ≥ 80% Phase 3 explained)
no longer collapse.

**Spec coverage**: depends on chosen fix; cite I/AP at commit time.

**Code changes**: depend on Phase 3 root cause. Some pre-loaded
fix candidates aligned with the hypotheses:

- If H1 (timeout cascade): treat send_invalidate -11 as **hard
  error**, propagate up to writer; writer re-broadcasts. Cascades
  break naturally because each producer retries individually
  rather than all giving up at the same wall-clock instant.
- If H2 (ring wraparound + stale resp_op_id): producer must clear
  resp_op_id BEFORE setting req_op_id, AND consumer must re-check
  req_op_id matches expected before storing resp_op_id (epoch
  tag in op_id high bits already handles this — verify).
- If H3 (blockpool exhaustion): increase per-host pool size based
  on max ops cap × avg ops-per-cell + safety; OR add background
  GC of retired blocks (more invasive).
- If H4 (cross-cell init residue): sweep script `chmod 666 +
  sleep 0.2` is insufficient; add explicit CXL region zero-out
  between cells (not just on host 0 init).
- If H5 (CPU starvation): pin dispatcher to dedicated core via
  `pthread_setaffinity_np`; OR cap T at 22-cores-reserved-for-
  system count (already done in iter-6A drop of T=86, may need
  drop T=64 too).
- If H6 (workload-d register saturation): add register batching
  OR dedicated register channel (more invasive — defer to iter-8A
  if H6 is sole cause).

**Validation experiment**:
1. Apply fix.
2. Re-run the 19 cells (deterministic + probabilistic) WITH probe.
3. Per cell: throughput should rise to within 50% of same-(workload,
   KV) median neighbor (i.e., no longer "collapsed" — at least in
   the same order of magnitude as healthy cells).
4. Anomaly scan post-fix: list any cell still showing < 1% of
   workload-peak throughput. Target: zero remaining collapsed cells.
5. G6 rw race test: violations=0 still holds.

**Success criterion**:
- 19 (or ≥ 80% of) collapsed cells now produce throughput ≥ 50%
  of same-(workload, KV) median.
- Zero NEW collapsed cells introduced (regression check on a
  random sample of healthy cells).
- G6 violations=0 still passes.

**Bottleneck check**: fix LOC ≤ 200; if larger, the fix is
itself an architectural change and Phase 5 needs a RAP per §XIII.

---

## Phase 5: Generalized doubling-ratio + anomaly scan + spec codify

**Goal**: re-sweep all 210 cells; verify zero unexplained outliers;
codify the "anomaly scan" step into spec so future iters don't
skip it (the iter-6A process failure).

**Spec coverage**: §13 iter-completion gate; §X new P4 enforcement.

**Code changes**:
- MODIFIED `scripts/run_iter6A_sweep.sh` → `run_iter7A_sweep.sh`:
  no parameter change; just re-run with the Phase 4 fix applied.
- MODIFIED `scripts/iter4A_redo_summarize.py`: add
  `anomaly_scan_section()` — for each (workload, KV), find peak
  Mops/s and flag any T-cell where Mops/s < 1% of that peak as
  "unexplained outlier"; print loud header in `gap_to_target.md`.
- MODIFIED `docs/scaling_ycsb_spec.md §13` iter-completion gate:
  add **gate 5: zero unexplained anomalies in `gap_to_target.md`**.
  Each anomaly must be either fixed (re-run shows no anomaly) OR
  explicitly explained in iter summary doc with cited root cause.
  No "single-rep noise" dismissal without backing 5-rep evidence.
- MODIFIED `docs/design_goals.md §X` enforcement framework: add
  **P4 (Process)**: "Sweep data is not 'documented' until every
  outlier is explained. Tagging an anomaly as 'noise' requires
  multi-rep verification (5 reps minimum). Skipping this step is
  the iter-6A precedent."
- MODIFIED `docs/scaling_ycsb_spec.md §3`: add **doubling-ratio
  is checked across all 5 workloads, not just workload-a**;
  iter-6A oversight where Phase 6 success was claimed on workload-a
  alone explicitly cited.

**Validation experiment**:
1. Full 210-cell sweep with Phase 4 fix.
2. anomaly_scan_section in `gap_to_target.md` reports **zero
   unexplained outliers**.
3. Doubling-ratio check across all 5 workloads (not just
   workload-a): each workload's pre-saturation T-doubling produces
   ≥ 1.5× throughput.
4. G6 violations=0.

**Success criterion**: 210/210 valid; 0 unexplained outliers;
doubling-ratio passes for **all 5 workloads**; spec §13 + §X +
memory updated.

**Bottleneck check**: sweep wallclock similar to iter-6A's 3.5h
(MAX_OPS=200000 unchanged; 0 timeouts expected post-fix → likely
faster).

---

## Cross-phase verification matrix

| Phase | Validates | LOC delta |
|-------|-----------|-----------|
| 1 | Reproducibility classification | 0 (re-run only) |
| 2 | Probe persistent dump | ~150 |
| 3 | Per-cell hypothesis A-B | 0 (probe-only) |
| 4 | Phase-3 fix | 50-200 (depends on diagnosis) |
| 5 | Anomaly-scan codified + spec gate 5 | ~80 (script + spec) |

Total expected: ~300-400 LOC; minimal protocol changes.

---

## Phase exit criteria (apply to every phase)

Standard CLAUDE.md + §X enforcement, plus iter-7A specific:

1. ✅ Phase validation experiments PASS on g3+g4
2. ✅ Bottleneck check produces in-budget number
3. ✅ Commit message references I/AP/G covered
4. ✅ No regression on prior phase validation
5. ✅ Spec drift audit (P2) finds no orphan implementation
6. **iter-7A specific**: any stage's diagnostic data goes into
   `docs/iter7A_phaseN_*.md` markdown — no "I think it's X"
   verbal claim without backing data file
7. **iter-7A specific**: no scope creep. If Phase 3 hypothesis
   testing reveals an unrelated bug, **document it as iter-8A
   backlog and continue**. Do NOT detour. (This is the iter-6A
   process failure: scope crept from "fix bi-modal" to "celebrate
   workload-c 94.8%" while ignoring 19 cells.)

---

## Hard scope constraints (this iter)

NOT in scope (deferred to iter-8A even if "easy" or "nice to
have" or "while we're here"):

- ❌ K-shard cache_dispatcher
- ❌ workload-a peak optimization above 7.6 Mops/s
- ❌ Re-enable 2 hash-diff tests (xhost_read, xhost_write)
- ❌ AP16 commit-msg hook regex extension (already partial in iter-6A)
- ❌ BucketLockTable removal
- ❌ ForwardEntry cacheline split (the ForwardRing equivalent of
  iter-6A's InvalEntry fix)
- ❌ Dropping T=64 if Phase 5 Hypothesis suggests CPU starvation;
  T grid stays {1, 2, 4, 8, 16, 32, 64} per iter-6A spec

The lone exception: if Phase 4 fix is structurally identical to
one of the deferred items (e.g., Phase 3 names "register channel
saturation" → fix happens to also be K-shard-style), the
identical fix is in-scope. Don't double-implement.

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Phase 1 finds all 19 are "non-reproducible flake" → no real bug | Low-Med | Document; close iter-7A early (saves 5+ phases). User decides if iter-7A should pivot to iter-8A backlog or close |
| Phase 3 finds 2+ root causes (e.g., H1 + H4 mixed) | Med | Phase 4 ships ONLY the fix for the larger subgroup; others go to iter-8A. Do NOT batch fixes |
| Phase 3 finds H7 (open) — no listed hypothesis matches | Med | Per QR7, persistence wins. Read probe data with fresh eyes; add gdb single-step on a failing cell; consider strace |
| Phase 4 fix fails A-B (collapsed cells stay collapsed post-fix) | Med | Phase 5 can't ship; revisit Phase 3 with corrected hypothesis. Do NOT push "partial fix" to sweep |
| Phase 5 sweep introduces NEW collapsed cells (regression) | Low | Per the §X new P4: anomaly scan catches; if new cells appear, the fix isn't truly fixing — back to Phase 3 |
| iter-7A wallclock > deadline | Med | Per QR7, no fixed budget; if approaching deadline ask user before scope-cut. Phase 1 alone (50 min) gives enough info to decide whether iter-7A is feasible at all |
| User asks for K-shard / peak optimization mid-iter | Low | Politely deflect to iter-8A — iter-7A's success requires the focus |
| Phase 4 fix accidentally breaks G6 strict-A | Low | G6 in Phase 4 success criterion; revert if violation |

---

## Open questions for review

| # | Question | Default if no answer |
|---|----------|---------------------|
| QR1 | Phase 1 reps = 5 vs 10 for cleaner classification? | 5 (matches spec §3 multi-rep policy) |
| QR2 | Phase 2 probe pre-allocation: 64 MB or 128 MB per thread? | 128 MB (200k ops × 16 stages = 76 MB; 128 MB has 1.7× headroom) |
| QR3 | Phase 3 A-B testing: every cell or sample 5 representative cells? | All deterministic cells get A-B; probabilistic cells get probe-only |
| QR4 | Phase 4 if multi-cause: ship 1 fix (drop coverage) vs ship N fixes (lose attribution) | 1 fix (largest cause subgroup); rest documented as iter-8A. **Do NOT batch fixes** — same lesson as iter-6A |
| QR5 | Phase 5 anomaly threshold: cell < 1% of workload-peak vs cell < 5%? | 1% (very loud bar; only catches "in the floor" not "merely low") |
| QR6 | Spec §13 gate 5 enforcement: hard fail or soft warn? | Hard fail (matches gates 1-4) |
| QR7 | Memory `feedback_scaling_ycsb_spec.md` update: include "anomaly scan is mandatory step" | YES; add to step 7 (iter-completion gate) |
| QR8 | If Phase 3 names root cause but Phase 4 fix turns out >300 LOC architectural change, ship as iter-7A or split? | Split. iter-7A ships ONLY a small fix (≤200 LOC) plus the spec/process changes; bigger architectural work moves to iter-8A. The "small fix" can be a workaround (e.g., disable the buggy path) |

---

## Quick stats

- **5 phases**; first 3 are pure diagnostic
- **~300-400 LOC** delta (probe upgrade + chosen fix + spec)
- **0 new features**, **0 backlog items**
- **1 named root cause** (or ≤ 2 subgroups) confirmed by A-B
- **1 process change**: §X P4 + §13 gate 5 codifying anomaly scan

---

## Pending user review

Please confirm/modify:

1. **Scope**: 19-cell-collapse diagnosis as the sole task — no
   K-shard, no peak optimization, no test re-enable, no cleanup.
   Agreed?
2. **Phase 1 reproducibility check**: 5 reps per cell — sufficient
   to classify deterministic vs noise?
3. **Phase 4 single-fix discipline** (QR4 default): ship one fix
   per phase; multi-cause means split to iter-8A, not batch.
4. **Spec codify** (Phase 5): adding "anomaly scan" as iter-completion
   gate 5; "single-rep noise" dismissals require multi-rep
   evidence — agreed?
5. **CLAUDE.md update** (parallel): should iter-6A's process
   failure (scope creep + confirmation bias on outliers) get its
   own cautionary precedent entry, alongside iter-2A's "result is
   unambiguous" descope?

After confirmation I start Phase 1.
