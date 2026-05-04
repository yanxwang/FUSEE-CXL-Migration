# Task plan — iter-8A — Protocol A: per-line latency attribution + force-collapse capture + named-cause fix

**Author**: Claude
**Date drafted**: 2026-05-03 (post-iter-7A 11:05 CDT)
**Status**: DRAFT — awaiting user review (open questions QR1-QR8)
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter8A-ubench]`, `[iter8A-probe]`, `[iter8A-eBPF]`, `[iter8A-capture]`,
`[iter8A-attrib]`, `[iter8A-rap]`, `[iter8A-fix]`, `[iter8A-sweep]`,
`[iter8A-blueprint]`)

**Spec**: `docs/design_goals.md §Protocol A (§I-XIII), AP16, G6, P4`
**Predecessors**:
- `docs/iters/iter7A_summary_20260503.md` (diagnosis-by-symptom only)
- `docs/iters/iter7A_phase3_diagnosis.md` (Phase 1+3 retry data)
- `docs/protocol_a_architecture_blueprint.md` (NEW — per-stage dictionary)

---

## Top-level principle: short-dense-fast (added 2026-05-03 per user)

**Never run a long sweep when a targeted experiment gives the same
signal in a fraction of the time.** Every Phase below follows
"smallest-cells × smallest-reps that answer the current question →
inspect feedback → decide next experiment". Specifically:

- **Phase 1 setup**: smoke each tool on 1 cell; verify; advance.
  No "validate at scale" before advancing.
- **Phase 2 force-collapse**: target ONE cell + capture on first
  collapse; do NOT run all 10 candidate cells × 5 retries
  prophylactically.
- **Phase 3 attribution**: synthesize from existing capture; do NOT
  request another full capture unless data is genuinely
  insufficient.
- **Phase 5 fix verification**: 1 cell × 5 reps on the named-cause
  cell + targeted regression check on the **2-3 cells most likely
  to be affected by the fix**. Do NOT re-run iter-7A's 210-cell
  sweep "just to confirm".
- **Phase 6 carve-out discharge**: target the **subset of 55 cells
  most likely to still collapse based on Phase 3 root cause** (not
  all 55); run those ×5 reps; rest discharged via reasoning if
  Phase 5 fix is structurally sufficient.
- **No standard 210-cell sweep at iter-end** unless the fix
  semantics demand it. Rationale: iter-7A already ran one; spec
  §13 iter-completion gate references the most-recent valid sweep,
  which iter-7A's 210-cell satisfies. iter-8A's value is the fix +
  attribution; revalidating untouched cells is waste.

**Rule of thumb**: if an experiment takes > 30 min wallclock, ask
"can a smaller experiment answer the same question?" before
launching. If yes, take the smaller one.

---

## TL;DR — exactly one task type

**iter-8A's only goal**: replace the symptom-level diagnosis iter-7A
shipped ("probabilistic transients") with **mechanistic per-line
latency attribution** that names — for every stage in
`docs/protocol_a_architecture_blueprint.md` Part II — the
**actual primitive** consuming time during both healthy and
collapsed runs.

**Why this and not optimization**: every previous "fix" iter-X has
shipped (iter-3A K-channel, iter-5A separate inval channel, iter-6A
cacheline split + 5 ms timeout cap) made symptoms move without
mechanistically locating the root. iter-8A breaks that pattern by
forbidding any optimization until the per-line table exists with
real measurements.

**Hard scope constraint** — explicit DEFERRED-to-iter-9A list:
- ❌ K-shard cache_dispatcher (architectural; iter-9A after iter-8A
  data shows whether it's actually warranted)
- ❌ ForwardEntry cacheline split mirror
- ❌ 2 hash-diff tests re-enable (xhost_read, xhost_write)
- ❌ BucketLockTable removal
- ❌ Variable-host-count dynamic re-shard

iter-8A's value is the **per-line attribution table + the named root
cause + the smallest fix that addresses it + the verification on
55 anomaly cells from iter-7A's gate 5 carve-out**. No volume of code.

---

## Plan overview

7 phases. Diagnostic-heavy: Phases 1-4 collect data, Phase 5
synthesizes, Phase 6 ships the fix, Phase 7 closes out.

| # | Phase | Type | Code? |
|---|-------|------|-------|
| 1 | CXL/DRAM primitive µbench (Sol-6) + probe RDTSCP upgrade (Sol-1) + bpftrace + perf setup (Sol-2/3) | Setup | Yes (~250 LOC probe + ~150 LOC µbench) |
| 2 | Force-collapse capture (Sol-5): reproduce a collapsed run with all 4 instruments running simultaneously | Data collection | No (use Phase 1 infra) |
| 3 | Per-line attribution table (Sol-4): synthesize Phase 2 data + Phase 1 baseline; populate Part II of blueprint with measured cells | Analysis | No |
| 4 | RAP for chosen fix targeting Phase 3 named bottleneck | Design | No (RAP doc only) |
| 5 | Apply fix | Implementation | Yes (≤ 200 LOC per QR8 of iter-7A reused; > 200 → workaround + iter-9A architectural) |
| 6 | 5-rep verify on iter-7A's 55 anomaly cells + full 210-cell sweep + iter-completion gate (G1-G6 + gate 5) | Verification | Sweep scripts only |
| 7 | Blueprint update + spec codify per-line-attribution as new validation gate | Codify | Doc only |

---

## Phase 1: Setup — µbench + probe upgrade + tracing tools

**Goal**: lay down ALL diagnostic infrastructure before any data
collection. Phase 2-3 should never have to wait for tooling.

**Spec coverage**: §VII AP16 (probe is observation only), §X P4
(measurement before claims).

### Sub-phase 1.A — CXL/DRAM primitive µbench (Sol-6)

**Code changes**:
- NEW `tests/cxl_primitive_bench.cc`: standalone microbenchmark
  measuring (per primitive, both single-host and 2-host concurrent):
  - `clflushopt` issue cost (back-to-back)
  - `clflushopt + sfence` complete-to-CXL cost
  - `mfence` cost
  - LD-CXL after flush (single line, sequential)
  - LD-CXL post-peer-store (cross-host coherence — measures CXL fabric round-trip)
  - ST-CXL + flush + sfence (single line, then peer reads)
  - `bump.fetch_add` on CXL atomic (single host)
  - `bump.fetch_add` ping-pong (cross-host on same atomic — should NOT happen in real path but baselines worst case)
  - `pthread_spinlock` uncontested + 2/4/8/16/32/64-way contention on local DRAM
  - Cache pool insert (DRAM hashmap with internal spinlock, contention sweep)

**Output**: `docs/iter8A_phase1_ubench/baseline.md` — table of per-primitive cost at typical-case + 8/64-thread concurrency. Used in Phase 3 attribution.

**Validation**: each primitive's median should be within 50% of
mlc-measured baseline (`docs/refs/g34_hw_baseline.md`); large
discrepancies indicate test harness bug.

### Sub-phase 1.B — Probe upgrade: RDTSCP + CPU-time + CPU-id (Sol-1)

**Code changes**:
- MODIFIED `src/cxl_probe.h`:
  - Replace `clock_gettime(CLOCK_MONOTONIC)` with **RDTSCP +
    LFENCE pair** for the wall-time field. RDTSCP serializes prior
    instructions (no OoO reorder around the probe entry); LFENCE
    after blocks reorder of subsequent instructions ahead of the
    probe.
  - Add per-probe **second timestamp**: `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`.
    Cost ~30 ns; only emitted at stage boundaries (≤ 16 per op),
    so overhead bounded.
  - Capture `TSC_AUX` (CPU id) from RDTSCP into the frame. If
    cpu_id changes between two consecutive probes within one stage,
    the thread was migrated — flag it.
  - Frame format upgrade (still 24 B aligned, repacked):
    - tag (8 B as before)
    - tsc_cycles (8 B from RDTSCP)
    - cputime_ns (4 B delta-from-thread-start, fits 4 sec at ns
      precision; for longer runs, a separate file header captures
      the absolute thread-start cputime so wall-clock sec can be
      reconstructed)
    - op_id (4 B truncated — high 4 B redundant since ring is per-thread)
- NEW `scripts/parse_probes_v2.py`: handles new format; converts
  RDTSCP cycles → ns using calibrated TSC frequency (calibrated at
  process start by measuring 1-sec interval with both RDTSCP +
  CLOCK_MONOTONIC); reports per-stage 5 columns: `wall_ns`,
  `cpu_ns`, `wall - cpu` (= preemption time), `migration_count`,
  N samples.
- NEW `scripts/probe_attribution.py`: given a dump dir + Phase 1.A
  baseline, generates per-stage attribution markdown table — for
  each stage, lists primitives expected, sums their µbench costs,
  compares to measured median, flags > 2× discrepancy.

**Validation**:
- Probe overhead < 5% of `protocol_a_local_test` baseline runtime
  (acceptance threshold). RDTSCP + LFENCE = ~12 ns; cputime_ns =
  ~30 ns; total ~42 ns × 24 probes/op × 1M ops = 1 sec overhead on
  10-sec run = 10% — borderline. **If overhead > 10%, the probe
  budget needs further reduction**: drop cputime_ns to every-4th-stage
  sampling; use buffered RDTSCP (read once, keep delta).
- `cpu_id` (TSC_AUX) populates correctly: spot check with
  `taskset -c 5` runs — every frame should have cpu_id == 5.

### Sub-phase 1.C — eBPF + perf setup (Sol-2/3)

**Code changes**: scripts only.

**NEW `scripts/iter8A_capture_bundle.sh`**: runs a target cell with
all four data sources active simultaneously:
1. `protocol_a_ycsb` with `FUSEE_PROBE_DUMP=...`
2. `perf record -g -p <pid> -o /tmp/perf_h<id>.data` for each of:
   primary worker process, ForwardResponder thread, CacheDispatcher
   thread (using `--tid` since they're threads inside primary)
3. `bpftrace -e '<sched_switch + futex script>' -o /tmp/bpf_h<id>.txt`
   captures every `sched_switch` event involving the target PIDs;
   computes per-thread off-CPU duration histograms
4. `mpstat -P ALL 1` to log per-core CPU utilization (sanity check
   for "is core X being saturated by IRQ during the cell")

Output bundle written to `docs/iter8A_capture_<ts>/{probe,perf,bpf,mpstat}/`.

**Validation**: dry-run the bundle on `protocol_a_local_test` (1
cell, 5k ops): all 4 outputs produced; bundle dir < 200 MB; total
overhead measured by comparing to non-instrumented run < 30%.

### Phase 1 success criterion (combined)
- µbench baseline.md exists for all primitives in
  `protocol_a_architecture_blueprint.md §III.1`
- probe v2 binary builds + smoke produces RDTSCP + cputime + cpu_id
  per frame
- capture_bundle script produces all 4 outputs

### Phase 1 bottleneck check
- µbench self-consistency: same primitive measured twice should
  agree within 10%
- Probe overhead < 10% on `protocol_a_local_test`

---

## Phase 2: Force-collapse capture

**Goal**: get a SIMULTANEOUS multi-instrument capture of a cell
that actually collapses. iter-7A's mistake was probing only healthy
runs; iter-8A must probe a collapse.

**Spec coverage**: §X P4 (measurement before claims), §VII AP16.

**The Heisenberg problem**: iter-7A observed that adding probes
pushed the system into the healthy regime, masking the collapse.
Phase 2's primary task is to **defeat this** by reducing probe
perturbation enough that the collapse still reproduces.

### Sub-phase 2.A — Test which cells collapse most reliably

**Code changes**: none.

**Validation experiment**: from iter-7A's 55 anomaly cells, pick
the top-10 most-frequently-collapsed (cross-reference iter-6A and
iter-7A both showed collapsed). Run each 5 times back-to-back
WITH probe v2 enabled. Record collapse rate per cell.

Output: `docs/iter8A_phase2_repro_rate.md` — per cell, 5-rep
collapse count + median throughput.

**Decision gate**: at least one cell must collapse at ≥ 60% rate
(3 of 5 reps) with probe enabled. If none reproduces:
1. Reduce probe to RDTSCP-only (drop cputime_ns); retry
2. Reduce to every-other-stage sampling; retry
3. Use deliberate-CPU-starvation control: run a `stress-ng
   --cpu N` background load on host primary's process group to
   simulate CacheDispatcher CPU starvation; retry
4. If still none reproduces, the bug is genuinely Heisenberg-sensitive
   — escalate to user (probe redesign needed; possibly need
   sample-mode tracing rather than on-every-event probes)

### Sub-phase 2.B — Capture the collapsed run

**Code changes**: none (Phase 1.C bundle).

**Validation experiment**: run the chosen "most reproducibly
collapsed" cell with `iter8A_capture_bundle.sh`. Confirm:
- SUMMARY line shows < 0.05 Mops/s (collapsed)
- All 4 data sources captured

If the very-first capture is healthy (Heisenberg), retry up to 5
times — keep the FIRST collapsed capture. Per QR1 user-reviewed
default: target up to 10 retries before escalating.

**Output**: `docs/iter8A_capture_<ts>/` — preserved binary + sweep
data. **This is the gold-standard data for Phase 3.**

### Sub-phase 2.C — Healthy comparison capture

Same cell, same parameters, but in a known-healthy run (e.g., right
after CXL device idle, fresh process). Same bundle script.

**Output**: `docs/iter8A_capture_healthy_<ts>/` — for diff against
the collapsed capture.

### Phase 2 success criterion
- 1 collapsed-run capture (full 4-source bundle) preserved
- 1 healthy-run capture (same 4-source bundle, same cell) preserved
- Both runnable through Phase 3 attribution scripts

### Phase 2 exit criterion
- Per QR7 of iter-5A/6A/7A pattern: persistence wins. No time
  budget. Phase ends only when the collapsed-run capture exists.
  Only escalate on physical impossibility (Heisenberg too severe
  to defeat with the 4 sub-phase 2.A tactics).

---

## Phase 3: Per-line attribution table (Sol-4)

**Goal**: synthesize Phase 1 baseline + Phase 2 captures into the
**named root cause**. Output is the attribution table that becomes
Part II baseline data in the blueprint.

**Spec coverage**: §X P4, §XIII RAP (drives Phase 4).

**Code changes**: `scripts/probe_attribution.py` (Phase 1.B) +
manual analysis docs.

**Procedure**:

1. Run `parse_probes_v2.py` on healthy + collapsed captures
   separately. Get per-stage `wall_ns`, `cpu_ns`, `wall - cpu`,
   `migration_count`, p50/p99/max per stage.

2. Run `probe_attribution.py` against Phase 1.A baseline. For each
   stage, get:
   - Expected typical (sum of primitives from baseline)
   - Measured healthy median + p99
   - Measured collapsed median + p99
   - Measured `wall - cpu` (= preemption time)
   - `migration_count`

3. Cross-reference perf data (`perf report` on each thread):
   - Worker: where does CPU time go? (`flush_line` calls, spin
     waits, work?)
   - ForwardResponder: hot lines?
   - CacheDispatcher: hot lines? Idle in PAUSE or busy?

4. Cross-reference bpftrace data:
   - Per-thread off-CPU duration histogram. If CacheDispatcher's
     histogram shows 100ms+ off-CPU events during the collapse, H5
     (CPU starvation) is confirmed.
   - Per-thread futex_wait counts.

5. Cross-reference mpstat:
   - During the collapse, is any core saturated by non-process
     work (IRQs, softirqs, kernel threads)? Pattern would suggest
     scheduler displacement.

**Output**: `docs/iter8A_phase3_attribution.md` — for each
stage in blueprint Part II, a row with:
- Expected (from µbench baseline)
- Healthy median / p99
- Collapsed median / p99
- preemption time (wall - cpu) collapsed
- migration_count collapsed
- Named cause for the gap (e.g., "I7 collapsed p99 = 5 ms = the
  cap; cpu_ns is < 5 µs; so CPU was waiting for ACK that never
  came; bpf shows CacheDispatcher had 47 ms off-CPU during this op
  → confirmed H5 CPU starvation")

**Phase 3 success criterion**: ≥ 80% of the wall-time gap (collapsed
- healthy) attributed to specific named causes per stage. The
**single dominant cause** (most aggregate wall-time gap explained)
becomes the named root cause for Phase 4 RAP.

**Phase 3 exit criterion**: same as iter-5A/6A/7A QR7 — persistence
wins. Phase ends only when ≥ 80% gap explained. Only escalate on
"data is contradictory and we need user judgment".

---

## Phase 4: RAP for chosen fix

**Goal**: per spec §XIII, write a full RAP for the optimization
that targets Phase 3's named root cause.

**Pre-loaded RAP candidates** (depending on Phase 3 named cause):

- **C-A: CacheDispatcher CPU pinning** (if H5 confirmed). Use
  `pthread_setaffinity_np` to pin CacheDispatcher to a dedicated
  core (one of the 22 reserved cores per spec). Optional: also
  give it `SCHED_FIFO` real-time priority. ~30 LOC.

- **C-B: Wait-for-slot-free timeout cap + slot-recycle protocol**
  (if I2/F3 wait-for-slot-free is the cascade amplifier). Worker
  waiting for slot > 5 ms gives up; slot is force-recycled with
  status = ABANDONED; consumer notes ABANDONED slots and skips them
  when ACKing. Likely > 200 LOC (slot recycle is non-trivial). Per
  iter-7A QR8: if > 200 LOC, ship workaround (e.g., shorter cap)
  in iter-8A and architectural fix to iter-9A.

- **C-C: Fail-loud on send_invalidate -11** (if Phase 3 shows
  silent timeouts are being absorbed without §I9 violation
  detection). Propagate -11 from `send_invalidate` up through
  `execute_write_local` to caller; caller MUST treat as a hard
  error. Possibly retry once. ~50 LOC.

- **C-D: K-shard CacheDispatcher** (if dispatcher is throughput-bound
  not CPU-starved, e.g., bpf shows dispatcher always running but
  ring saturates). Architectural; > 200 LOC; iter-9A first task.
  iter-8A skips.

- **C-E: ForwardEntry cacheline split** (if Phase 3 shows ForwardRing
  ping-pong is the bottleneck — same pattern as iter-6A's InvalRing
  fix). ~30 LOC if mirrors InvalRing fix. Could be iter-8A scope.

- **C-F: cputime-aware backoff in spin loops** (if Phase 3 shows
  workers waste CPU spinning while dispatcher needs the cycle).
  Add `sched_yield()` after N spin iters in `wait-for-slot-free`
  and `spin_wait_resp`. ~20 LOC.

**RAP format** (per spec §XIII): STATE / ATTACK VECTORS (≥ 6 from
6 categories: PERFORMANCE, CORRECTNESS, GENERALITY, COMPLEXITY,
PRIOR ART, IMPLEMENTATION FEASIBILITY) / ABLATION CHECK
(≥ 2 alternatives ruled out) / PRIOR ART CHECK / VERDICT / DECISION.

**Output**: `docs/iter8A_phase4_rap.md`.

**Decision rule (QR4 of iter-7A locked, reused here)**: ship ONLY
the fix for the LARGEST-attributed root cause. Other named causes
go to iter-9A backlog with their own RAP-ready hypothesis.

---

## Phase 5: Apply the fix

**Goal**: implement the Phase 4 RAP-validated fix.

**Spec coverage**: depends on chosen fix (cite I/AP/G at commit time).

**Code changes**: depend on chosen fix. Per QR8 of iter-7A: ≤ 200
LOC; if Phase 3 named root cause requires > 200 LOC architectural
change, iter-8A ships a workaround per QR8 + the architectural fix
moves to iter-9A first task.

**Validation experiment**:
1. Smoke: 5 reps of the cell that was force-collapsed in Phase 2.
   ≥ 4 of 5 reps no longer collapsed.
2. Probe attribution rerun: the targeted stage's wall-cpu gap
   drops ≥ 5× compared to Phase 2 collapsed capture.
3. G6 rw race test: violations=0 still holds (no §I9 regression).

**Phase 5 success criterion**: stage wall-cpu gap ≥ 5× reduction;
G6 still passes.

---

## Phase 6: Verification — targeted, NOT full sweep (per top-level principle)

**Goal**: confirm the Phase 5 fix actually fixes the named root cause,
discharge iter-7A's gate 5 carve-out for the cells most likely affected,
and detect regressions on the cells most likely affected by the
fix's code path. **No 210-cell standard sweep this iter.**

**Spec coverage**: §13 gate 5 (anomaly verification, scoped subset),
§IX G6.

**Rationale for not running full 210-cell sweep**:
- spec §13 iter-completion gate references most-recent valid sweep
  → iter-7A's `g34_scaling_ycsb_20260503_060910/` already satisfies
- the cells we have NOT touched semantically (workload-c read-only,
  workload-d at non-collapse Ts, etc) cannot regress from a
  Phase 5 fix targeting CacheDispatcher / wait-for-slot-free / similar
- a 3-4h re-sweep would mostly re-confirm what iter-7A already showed
- per user 2026-05-03: short-dense-fast experiments preferred

### Sub-phase 6.A — Targeted carve-out discharge

**Code changes**: NEW `scripts/iter8A_targeted_verify.sh` — runs a
SCOPED list of cells × 5 reps each (NOT all 55).

**Cell selection (SCOPED, not all 55)**:
- The 1 force-collapsed cell from Phase 2 (the gold-standard test
  case) — 5 reps; must show ≥ 4 of 5 OK post-fix.
- The cells iter-7A Phase 1 retry classified as the "deterministic"
  bucket (the 3 d-kv1024 T=32/64 cells, all of which on iter-7A
  Phase 3 retry then turned probabilistic) — 5 reps each; expect
  ≥ 4 of 5 OK.
- 3-5 cells from iter-7A's 55 anomaly set whose root cause Phase 3
  attributed to the SAME mechanism we just fixed — 5 reps each.

**Total**: ~5-9 cells × 5 reps = 25-45 runs. Wallclock estimate
≤ 30 min. Per top-level principle: short.

**Validation**:
- Force-collapsed cell (Phase 2) must show ≥ 4/5 OK.
- Each "structurally-related-to-fix" cell improves: anomaly rate
  drops vs iter-7A's per-cell history.
- Cells that DON'T improve are documented as "fix didn't address
  this cluster" → iter-9A backlog item, not Phase 5 RAP revisit
  unless they're the dominant Phase 3-attributed cluster.

### Sub-phase 6.B — Targeted regression check

**Cell selection (the "did fix break anything" set)**:
- 1 cell per workload at peak-T (e.g., workload-c KV=1024 T=64,
  the iter-6A 18.95 Mops/s peak) — 1 rep each.
- 1 cell per workload at low-T (T=4) where the fix is most likely
  to NOT help but should not hurt — 1 rep each.

**Total**: 5 wl × 2 T = 10 cells × 1 rep = 10 runs. Wallclock
estimate ≤ 10 min.

**Validation**: each cell within 50% of iter-7A's measured value
(generous noise band; we're looking for catastrophic regression,
not micro-perf changes).

### Sub-phase 6.C — G6 rw race test (single run)

`tests/protocol_a_rw_race_test 1000` — must report violations=0.

### Phase 6 success criterion (combined ≤ 1 hour wallclock)
- Phase 2 force-collapsed cell post-fix: ≥ 4/5 OK
- Carve-out subset improved post-fix
- 10-cell regression check: no catastrophic delta
- G6: violations=0

If any sub-phase fails, Phase 5 RAP revisited; do NOT push partial
fix to summary.

---

## Phase 7: Blueprint update + spec codify

**Goal**: keep `protocol_a_architecture_blueprint.md` current per
its Update Protocol; codify per-line attribution as a recurring
deliverable type.

**Code changes**:
- MODIFIED `docs/protocol_a_architecture_blueprint.md`:
  - Part II: every stage's "healthy baseline" line replaced with
    Phase 3 measured median (RDTSCP, healthy run). Add `// iter-8A
    measured` annotation.
  - Part III.1: CXL primitive cheat sheet replaced with Phase 1.A
    µbench data.
  - Part III.3: Phase 5 fix's effect on a failure mode (e.g., if
    we shipped C-A CPU pinning, document its effect on the
    "CacheDispatcher CPU starvation" entry).
- MODIFIED `docs/scaling_ycsb_spec.md`: add gate 6 — "per-stage
  attribution data must be recorded for any sweep that reports
  scaling-shape regression on any workload" (turns Sol-4 attribution
  into a recurring deliverable, not an iter-8A one-off).
- MODIFIED `docs/design_goals.md §X`: add P5 — "Performance claims
  require per-stage attribution data; 'measured X µs' without naming
  the primitive responsible is not an explanation. Attribution
  table = the new artifact."

**Validation**: blueprint v2 is internally consistent (every
stage's measured baseline ≤ µbench expected baseline within 5×);
spec text reads correctly.

---

## Cross-phase verification matrix

| Phase | Validates I/AP/G | LOC delta |
|-------|------------------|-----------|
| 1 | µbench correctness; probe overhead | ~400 (probe v2 + µbench + capture script) |
| 2 | force-collapse with instruments enabled | 0 |
| 3 | per-stage attribution synthesis | 0 (analysis docs only) |
| 4 | RAP for chosen fix | 0 (RAP doc only) |
| 5 | chosen fix; G6 still passes | depends; ≤ 200 per QR8 |
| 6 | 55 anomaly cells discharged + standard sweep + gate 5 + doubling-ratio all 5 wl | ~50 (scripts) |
| 7 | blueprint Part II/III current; spec §X P5 + spec gate 6 | ~100 (docs) |

---

## Phase exit criteria (apply to every phase)

Standard CLAUDE.md + §X enforcement, plus iter-8A specific:

1. ✅ Phase validation experiments PASS on g3+g4
2. ✅ Bottleneck check produces in-budget number
3. ✅ Commit message references I/AP/G covered
4. ✅ No regression on prior phase validation
5. ✅ Spec drift audit (P2) finds no orphan implementation
6. **iter-8A specific**: any stage's diagnostic conclusion goes
   into `docs/iter8A_phaseN_*.md` markdown — no "I think it's X"
   verbal claim without backing data file
7. **iter-8A specific**: no scope creep beyond Phase 4's named
   root cause. If Phase 3 reveals an unrelated bug, document as
   iter-9A backlog and continue.
8. **iter-8A specific**: Phase 1 must complete BEFORE Phase 2
   starts (probe v2 + µbench needed for Phase 2 bundle); all
   other phase gates standard.

---

## Hard scope constraints (this iter)

NOT in scope (deferred to iter-9A):
- ❌ K-shard cache_dispatcher
- ❌ workload-a peak optimization
- ❌ 2 hash-diff tests re-enable
- ❌ BucketLockTable removal (-2.5 GB)
- ❌ Variable-host-count dynamic re-shard
- ❌ Spec changes touching §I/§II/§V (semantic invariants)
- ❌ Anything not directly traceable to a Phase 3 attribution row

The lone exception per QR4 below: if Phase 4 RAP names a fix that
is structurally identical to a deferred item (e.g., Phase 3
attributes the cause to ForwardEntry ping-pong → fix mirrors
iter-6A's InvalEntry split → C-E candidate), the identical fix is
in-scope.

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Phase 1 µbench numbers don't agree with mlc baseline (calibration broken) | Low-Med | Phase 1.A acceptance gate: median within 50% of mlc; if not, debug bench harness before Phase 2 |
| Phase 1 probe overhead > 10% — Phase 2 reproducibility may fail | Med | Phase 1.B fallback: drop cputime_ns; use buffered RDTSCP; if still > 10%, escalate user (perhaps need sample mode) |
| Phase 2 cannot reproduce collapse with probe enabled (Heisenberg too strong) | Med-High | 4 fallback tactics in §2.A; if all fail escalate user |
| Phase 3 finds 2+ comparable root causes | Med | Per QR4 of iter-7A: ship ONLY largest; others to iter-9A |
| Phase 3 finds NO single dominant cause (latency uniformly distributed across stages) | Low-Med | Re-instrument with finer probes within suspect stages (e.g., split W7 into W7.1-W7.3); rerun Phase 2 |
| Phase 5 fix passes per-stage 5× reduction BUT 55-cell verification still shows persistent collapses | Med | Phase 6 reveals fix is incomplete; Phase 5 RAP revisited; possibly need second fix in iter-9A |
| Phase 6 wallclock > 1h (now ≤ 30 min carve-out + ≤ 10 min regression + G6) | Low | Phase 6 reshaped per user 2026-05-03 to short-dense-fast; full sweep dropped (iter-7A's already satisfies §13 gate). If somehow > 1h, escalate user. |
| Phase 5 fix accidentally breaks G6 strict-A | Low | Phase 5 success criterion includes G6 check; revert if violation |
| User requests K-shard / peak optimization mid-iter | Low | Politely deflect to iter-9A — iter-8A's success requires the focus |

---

## Open questions for review (QR1-QR8)

| # | Question | My recommendation |
|---|----------|-------------------|
| QR1 | **Phase 2 force-collapse strategy**: try (A) workload-d KV=1024 T=64 cache=on (iter-7A's most-collapsed cell); fall back to (B) deliberate stress-ng CPU starvation as control? | **(A) first**, up to 10 retries; if fails, fall back to (C) stress-ng + lower probe density (drop cputime_ns from every-stage to every-4th-stage). Rationale: real-world reproduce > artificial reproduce; but artificial is a valid backstop. |
| QR2 | **Probe overhead acceptance threshold** | **10% on `protocol_a_local_test`**. Above 10% → Phase 1.B fallback (drop cputime_ns / sample mode). Rationale: 10% leaves enough headroom that Phase 2 cell-level wallclock pattern remains observable. |
| QR3 | **55-cell × 5-rep verification mode** — strict (every cell × 5 reps no matter what) or pragmatic (only cells STILL collapsing in iter-8A first sweep need 5-rep)? | **Pragmatic + scoped further per user 2026-05-03**: only ~5-9 cells most-likely-affected by the Phase 5 fix get 5-rep; remainder discharged via reasoning. Total ≤ 45 runs ≤ 30 min. iter-7A Phase 1 already showed 22/25 self-resolved on retry. |
| QR4 | **Phase 4 fix candidates C-A through C-F** — should I pre-implement the smallest one (C-F sched_yield) so we can ablate "yield vs no-yield" as a control during Phase 2 capture? | **No** — keeps Phase 1-3 strictly diagnostic. Pre-implementing biases the data. Wait until Phase 4 RAP names what to ship. |
| QR5 | **CPU pinning candidate (C-A) fix scope** — if Phase 3 names CPU starvation, should iter-8A pin BOTH ForwardResponder and CacheDispatcher, or just CacheDispatcher? | **Just CacheDispatcher** for iter-8A. Rationale: only one root cause per iter (per QR4 of iter-7A); ForwardResponder pinning is iter-9A. |
| QR6 | **Wait-for-slot-free timeout cap (C-B)** — currently no timeout = cascade amplifier. If Phase 3 names this as root cause, ship which workaround? Options: (a) reduce timeout to 1ms with no slot-recycle (workers fail-loud on timeout); (b) add slot-recycle protocol (≥ 200 LOC, would push iter-9A) | **(a) workaround in iter-8A; (b) iter-9A architectural**. Rationale: per QR8 of iter-7A, ≤ 200 LOC fix in iter-8A; (a) is ~10 LOC. |
| QR7 | **Phase 7 spec gate 6 (per-stage attribution)** — make it HARD FAIL like gate 5, or SOFT WARN? | **Soft warn first iter; HARD FAIL after iter-9A** demonstrates Sol-4 attribution can be auto-generated reliably. Rationale: hard-fail on a measurement requirement before the tooling is mature creates false barriers. |
| QR8 | **Phase 6 wallclock risk** — if 6.A + 6.B exceeds budget, drop which first? | **Phase 6 reshaped per user 2026-05-03 to ≤ 1h total** (≤30 min carve-out targeted + ≤10 min regression + G6). Full 210-cell sweep dropped this iter; iter-7A's already satisfies spec §13. If even reshaped Phase 6 exceeds budget, escalate user. |

---

## Quick stats

- **7 phases**, first 4 are pure diagnostic
- **~750 LOC** total delta (probe v2 + µbench + capture scripts +
  Phase 5 fix + Phase 6 verify scripts + Phase 7 docs)
- **0 architectural changes** unless Phase 3 explicitly names one
- **55 cells × 5 reps** carved-out from iter-7A discharged in Phase 6.A
- **1 named root cause** confirmed by Phase 3 attribution + Phase 4 RAP
- **NO optimization phase ships before per-line attribution table
  exists with measured data**

---

## Awaiting deadline

All design decisions captured; QR1-QR8 await user.

CLAUDE.md cautionary precedents #1 and #2 apply directly to iter-8A:
no scope creep, no descope without escalation, every claim has
backing data file, anomaly verification before dismissal.

`docs/protocol_a_architecture_blueprint.md` is the iter-8A's primary
update target — Phase 7 commits the measured per-stage baselines
into Part II as the new authoritative reference for iter-N+ probes.
