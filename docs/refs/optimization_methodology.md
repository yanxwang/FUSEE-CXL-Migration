# Iterative Systems Optimization — Methodology

**Status**: Layer 0 reference (alongside `design_goals.md` and
`scaling_ycsb_spec.md`). Distilled from five iterations of
Protocol C optimization (2026-04-23 → 2026-04-25), generalised
to apply to any protocol / subsystem optimization in this repo
or beyond.

**Audience**: The next person (human or Claude) starting an
optimization push on Protocol A, Protocol B, or any future
subsystem. Read top-to-bottom once, then refer back to specific
§ when planning a new iter.

---

## Contents

1. The non-negotiables
2. The per-iter loop (5 phases)
3. Diagnostic methods (the toolbox)
4. Hypothesis discipline
5. Ceiling analysis (the 3-layer stack)
6. Anti-patterns (mistakes we made; don't repeat)
7. Document trail per iter
8. Triggers for stopping a focus topic
9. Recurring design patterns

---

## 1. The non-negotiables

These are not negotiable. They override schedule pressure, deadline
pressure, "let's just ship this" pressure.

### 1.1 Compare every result to the project bar, not to "last week"

A "3 ×–5 × improvement" is **not a stopping condition**. The
stopping condition is a **defined absolute target** (for FUSEE-CXL:
20 Mops/s on YCSB workloads A and C — see `docs/design_goals.md`).
Every iter summary computes the **gap to the bar** explicitly. If
no defined target exists, define one before starting.

### 1.2 Diagnose before optimizing

Never write optimization code without **first** showing
quantitatively (latency decomp, sweep, microbench) which subsystem
is the binding constraint. "I think it's the lock" is not a
diagnosis; "lock-acquire p50 = 4.2 µs out of 9 µs total at T=64
under workload A from `docs/iters/latency_decomp_*.md`" is.

### 1.3 If a hypothesis fails, REVISE the diagnosis explicitly

When an optimization doesn't deliver the expected gain (e.g.
iter-5 multi-flusher giving +5 % instead of expected +30 %),
**stop and write a new diagnosis section** before proposing the
next change. Do not silently move to a new optimization on the
same fuzzy hunch. The revised diagnosis is itself a deliverable.

### 1.4 One iter = one task_plan + one summary

Per `docs/CONTRIBUTING.md` §1. No iter is "done" until both docs
exist, the runs_index has its row, and `fusee_cxl_progress.md`
tail has its section.

### 1.5 Execute every planned phase within the deadline

Within a user-given deadline, **execute every planned phase to
completion**. Do NOT use time judgment to drop, defer, or shrink
planned work. The user's planning unit is **hours**, plans are
estimated in **days** — empirically the day-estimates fit in the
hour-windows; assume they will unless physically impossible.

Spare time at the end of an iter goes to:
- **More verification**: extra cells, extra decomp instrumentation,
  controlled-comparison re-runs of cells where two factors changed.
- **Strengthening / falsifying hypotheses**: e.g. if Phase-3
  decomp suggests a new dominant stage on the *receiver* side,
  add receiver-side instrumentation (this is the "subjective
  initiative" we want — toward MORE rigour, not LESS work).
- Modest deadline overruns of 30–60 min to finish a planned
  measurement.

Unacceptable (violations, not judgment calls):
- Stopping early because "result is obvious / unambiguous."
- Deferring planned phases to the next iter without first
  attempting them within the deadline.
- Substituting an ad-hoc smoke test for a planned full sweep.
- Treating hypothesis falsification as a stop-trigger. Falsification
  is a result (§4.3), not a halt; the planned sweep + decomp still
  ship and become the data that drives the next iter.

If a phase is genuinely impossible within the deadline (testbed
unreachable, hardware limit reached, blocking dependency on user
input), **ask the user before descoping**, do not unilaterally
pick a subset.

**Deadline semantics + no-time-estimates rule** (added 2026-04-27):

- Deadline = "fully push planned phases + autonomously add
  verification experiments." User-given hour-windows have
  empirically always been sufficient for day-scale plans.
- Modest overrun (≤ ~1.5 h) is acceptable when used to finish
  planned work or run unplanned-but-warranted experiments.
- Spare time priority: more verification > additional
  controlled-comparison cells > re-running noisy cells >
  documentation polish. **NEVER** "decide what else to descope."
- **No hour/day time estimates in any plan deliverable**. They
  bias descope (iter-2A precedent: "10 d plan in 5 h window"
  psychologically triggered descope; the work would have fit
  fine). Use qualitative complexity language only ("largest
  rewrite of dispatch path", "exceeds typical iter scope —
  consider split?"), never numeric.
- Internal effort ranking is fine; not surfaced in user docs.

**Cautionary precedent — iter-2A** (2026-04-26): Phase 1 finished
at 06:27, deadline was 11:59 AM (5h 31min remaining). Phases
2–8 were descoped with the rationale "result is unambiguous, not
worth 3+ hours of testbed time." Two violations:
1. The descope rationale was based on **unmeasured assumption**:
   the "single-receiver fan-out is the new bottleneck" claim used
   to justify skipping Phase 4 sweep + Phase 5 decomp had ZERO
   receiver-side instrumentation backing it; per-entry receiver
   cost was hand-calculated (~1.5–2 µs by adding component costs),
   never actually measured.
2. The right behavior with 5h 31min spare: run the planned 80-cell
   Phase-4 sweep (3–4 h chained) AND add receiver-side decomp
   instrumentation in parallel. Both fit. The "diagnostic is
   unambiguous" claim should have been the OUTPUT of running
   them, not the EXCUSE to skip them.

iter-3A starts by remediating this — first task before any
optimisation is "add receiver-side per-entry instrumentation +
queue-depth probe + re-run T=4 PHR=1 cell + archive raw log."

### 1.6 Cite ceiling layer in every BW claim

When you say "X is bandwidth-bound", say which **layer ceiling**
you are referencing — Layer 1 mlc raw, Layer 2 user-space NT, or
Layer 3 protocol path. See `docs/design_goals.md` § "Hardware
baseline (3-layer ceiling stack)". This single discipline catches
the iter-4 → iter-5 ceiling correction (22 GB/s estimate vs
12.5 GB/s measured).

---

## 2. The per-iter loop (5 phases)

```
┌── Phase 0: PLAN ──────────────────────────────────────────────┐
│  task_plan_<date>_<topic>.md                                   │
│  - decision table (Q1..QN, defaults + user lock-in)            │
│  - phased breakdown (file-level changelist with verify check)  │
│  - success criteria (numeric, against the bar)                 │
│  - risk matrix                                                  │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌── Phase 1: BASELINE + CEILING ─────────────────────────────────┐
│  Reproduce last-iter numbers (sanity check), measure current   │
│  cell against the bar, compute Layer-3 utilisation against     │
│  Layer 2. Output a table:                                       │
│   workload | current peak | bar | Layer-3 util | gap to bar   │
│  Decide: are we latency-bound (util < 0.5) or BW-bound (>0.7)? │
│  This determines which optimizations are worth attempting.     │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌── Phase 2: DIAGNOSTIC ─────────────────────────────────────────┐
│  Latency decomp via FUSEE_LATENCY_DECOMP=1 (or analogous       │
│  instrumentation). Lock anatomy via instrumented LFM (4-probe  │
│  rdtsc / clock_gettime). Stage-by-stage p50/p99 table.         │
│  Output a doc:                                                  │
│    docs/iters/latency_decomp_<topic>_<date>.md                 │
│  Identify the **dominant stage** quantitatively. Verify it is  │
│  consistent with the Phase-1 latency-vs-BW classification.     │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌── Phase 3: HYPOTHESIS + IMPLEMENTATION ────────────────────────┐
│  State the hypothesis explicitly:                               │
│   "Reducing X by Y will lift workload Z by W Mops/s, because   │
│    X is currently Y of the per-op latency budget."             │
│  Implement; commit with [<tag>] prefix per CONTRIBUTING §2.4.  │
│  Smoke test before sweep.                                       │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌── Phase 4: SWEEP ──────────────────────────────────────────────┐
│  Per scaling_ycsb_spec.md.                                      │
│  Always include a control row matching the prior iter's config │
│  (regression check). If sweep matrix is large, sequence via    │
│  chain script overnight; monitor with run_in_background.       │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌── Phase 5: SUMMARY + FALSIFY ──────────────────────────────────┐
│  - Headline tables: peak per (cell), gap to bar.               │
│  - Audit success criteria from Phase 0. Mark each ✅/❌.       │
│  - **If hypothesis failed: write a "Diagnosis revised" section │
│    explicitly stating the new bottleneck**, before proposing   │
│    iter-N+1.                                                    │
│  - Update progress.md tail (~50 lines), runs_index (1 row),    │
│    memory project_<topic>.md digest.                            │
│  - Carry deferred items forward as named follow-up.            │
└────────────────────────────────────────────────────────────────┘
```

**Time budget** typical: Phase 0 = 0.5 d, Phase 1 = 0.5 d,
Phase 2 = 0.5–1 d, Phase 3 = 2–5 d, Phase 4 = 0.5 d (chained
overnight), Phase 5 = 0.5–1 d. **Total: 4–8 d per iter.** Iters
that take more than 10 d should be split.

---

## 3. Diagnostic methods (the toolbox)

Listed roughly in order of "cheap → expensive" and "narrow →
broad". Pick the cheapest tool that answers the question.

### 3.1 SUMMARY.log peak comparison (cheap, narrow)

Just run a sweep, compare peaks vs prior iter's sweep at the same
config. Useful for: regression detection, controlled comparison
(N=1 vs N=2). Limitation: doesn't tell you **why** numbers moved.

### 3.2 Latency decomp instrumentation (medium, narrow)

Pattern from `src/cxl_kv_ops_C.cc`:
```c
#if FUSEE_LATENCY_DECOMP
  uint64_t t0 = rdtsc();
  // stage 1
  uint64_t t1 = rdtsc();
  // stage 2
  uint64_t t2 = rdtsc();
  ...
  record_stage_ns(STAGE_LOCK,  t1 - t0);
  record_stage_ns(STAGE_SCAN,  t2 - t1);
#endif
```
Build with `-DFUSEE_LATENCY_DECOMP=1`, run, dump per-stage p50/p99
into `docs/iters/latency_decomp_<topic>_<date>.md`. Use to
identify **the dominant stage** of the critical path.

Caveat: `rdtsc` on cross-host ops is local-only — it does not
measure the CXL roundtrip portion. Use `clock_gettime(CLOCK_MONOTONIC)`
if you need wall-clock accuracy across NUMA boundaries.

### 3.3 Lock anatomy (medium, narrow)

Pattern from iter-3 phase-1: instrument the LFM lock primitive
itself with 4 probes (`local_store / peer_scan / cont_wait /
enter_cs`) — output in
`docs/iters/latency_decomp_C_iter3_lock_anatomy_*.md`.
Distinguishes **acquire-physics** (intrinsic to the lock algorithm)
from **queueing** (caller-side contention). The "queueing dominates
acquire-physics" signature ruled out replacing LFM with MCS in
iter-3 phase-1.5.

### 3.4 Microbench (medium, narrow)

Standalone binary that exercises one subsystem in isolation.
Examples:
- `tests/cxl_dualhost_bw_bench.cc` (iter-5 M1) — measures the BW
  ceiling **without** any FUSEE protocol overhead.
- `tests/cxl_blockpool_test.cc` (iter-4) — verifies pool
  primitive correctness independent of the K-V protocol.

Use a microbench when you want to **bound** a layer of the system
without being confused by a higher layer. Layer-2 vs Layer-3
ceiling separation in `design_goals.md` is exactly this discipline.

### 3.5 mlc raw hardware probe (medium, broad)

`/tmp/mlc/Linux/mlc --latency_matrix` and `--bandwidth_matrix
[-W2]` on g3 or g4. Requires switching `dax0.0` to system-ram
mode. Use when you need to know the **device link upper bound**
that no software path can exceed. See `design_goals.md` § Layer 1.

### 3.6 Uncore counter byte-account (expensive, narrow) — UNUSED

Originally proposed as iter-5 M2 to measure cross-host
amplification. Replaced by calc-based M2 (M1 measured aggregate /
expected per-op bytes). Re-enable if a future iter has a
hypothesis specifically about hidden CXL traffic (snoop, coherence
sideband) that calc-based methods can't distinguish.

### 3.7 Stage-decomp at varying parameter (expensive, broad)

Iter-5 M3 pattern: rerun the same workload at varying `vsize`
or `T` and watch which stage's latency scales. Use to separate
**fixed costs** (don't scale) from **variable costs** (scale with
input size). Output: a "constant cost ≈ X µs" claim, e.g. iter-5's
"8 µs floor on hot bucket".

### 3.8 Hardware perf counter inline (expensive, broad)

`perf stat -e <events>` to count cycles, cache misses, etc.
Useful as a last resort when other methods leave the bottleneck
ambiguous. Beware: counter availability and naming vary by CPU
generation; budget time for counter discovery.

---

## 4. Hypothesis discipline

### 4.1 Every iter has exactly one primary hypothesis

State it in Phase 0 as "I believe X is the binding constraint;
optimization Y will lift workload Z by W Mops/s." Anything else
is hand-waving.

### 4.2 The hypothesis must be falsifiable

Bad: "multi-flusher will help A."
Good: "multi-flusher will lift workload A peak from 17 to ≥ 25
Mops/s at T=64 cache=on, by parallelising the per-bucket
`bump_epoch` rate. If A peak < 22 at every (kv, N) cell, the
hypothesis is false."

### 4.3 Falsification is a result, not a failure

Iter-5's multi-flusher hypothesis was **falsified** (A peak 19.35
< 25). That is **valuable** — it ruled out a whole class of
"more flushers" optimizations and sharpened the next iter's
target to hot-bucket producer-side serialisation. The summary doc
must say "hypothesis falsified" out loud, in those words, with
the revised diagnosis.

### 4.4 Don't compound multiple optimizations in one iter

If iter-5 had bundled multi-flusher AND value-cache, a falsified
A bar would be ambiguous: was it multi-flusher's fault, or
value-cache's, or both? **One primary hypothesis per iter.** Other
optimizations land as side effects (e.g., refactors, cleanup) but
are not the test subject.

### 4.5 The success criterion comes from the bar, not from the
optimization

iter-5 success criterion 3: "A peak ≥ 25 Mops/s." That number
came from the 20 Mops/s bar plus ~25 % headroom. It did **not**
come from "we expect 30 % gain". Tying the criterion to the bar
keeps optimization honest — partial gains that don't move the bar
are partial.

---

## 5. Ceiling analysis (the 3-layer stack)

See `docs/design_goals.md` § "Hardware baseline" for the canonical
numbers and reasoning. Quick reference for iter planning:

```
        Layer 1 (mlc raw)                         51.6 GB/s/host
                ↓ (devdax loses channel interleave + naive AVX kernel + sfence)
        Layer 2 (user-space NT-stream, M1)        12.5 GB/s/host
                ↓ (FUSEE protocol: lock + flush + bump_epoch + clflushopt)
        Layer 3 (FUSEE protocol-derived)          variable per (workload, vsize)
```

**Use Layer 2 as the operative ceiling** for FUSEE write throughput.
Layer 1 is unreachable from FUSEE's regime; comparing FUSEE numbers
to Layer 1 **mis-classifies** workloads as latency-bound when they
are actually BW-bound (this is what iter-4 did with the wrong
22 GB/s estimate).

**Layer-3 utilisation table** is the iter-priority signal:
- util < 0.4 → latency-bound; protocol optimisation has big upside
- 0.4 ≤ util ≤ 0.7 → mixed; protocol opt + byte-reduction both help
- util > 0.7 → BW-bound; only byte-reduction or layer-mode change moves the needle

This classification answers Phase 1 "what kind of bottleneck do
we have?" directly.

---

## 6. Anti-patterns (mistakes we made; don't repeat)

### 6.1 "3 × over baseline = done"

iter-1 ended at A = 3.41 (3.16 × baseline) — a great-sounding
gain that was **6 × short of the 20 Mops/s bar**. Don't celebrate
ratios; cite gap-to-bar in absolute units.

### 6.2 Wrong ceiling estimate

iter-4 used a 22 GB/s ceiling guess (no microbench backing) →
classified kv-1024 as 0.82-of-ceiling = "BW-saturated" → would
have under-prioritised multi-flusher iter-5 if the ceiling had
actually been 0.82. Iter-5 M1 measured 12.5 GB/s/host → revised
to 0.73 = still BW-bound but with 25 % headroom. **Always
microbench the ceiling before invoking it.**

### 6.3 Run-to-run variance ignored

iter-3 → iter-4 kv-8 N=1 cells should be byte-for-byte identical
(same code path) but drifted ±15 % (A 17.05 → 13.94). The
explanation is "NUMA / thermal / runner stack" but it was never
formally controlled. **For comparisons that depend on small
deltas (e.g. iter-5's +5 % multi-flusher gain on A), always run
the comparison cells in the same time window.**

### 6.4 "Going forward" planning that pre-empts data

CONTRIBUTING §3 originally said "future files go here, existing
files stay" — this language created a documentation drift where
iter-5 deliverables ended up in flat `docs/` because the rule
spoke about the future, not the present. **Decide layout
retroactively, not for an imagined future.**

### 6.5 Compounding optimizations (see §4.4 above)

iter-3 phase-3 bundled `[read-singleshot]` + `[flush-collapse]` +
`[route-seq]` + `[micro-batch]` in one sweep. The aggregate gain
was great (B 10.54 → 33.37, +217 %), but no one knows which of
the four contributed how much. If a future iter regresses, we
can't bisect.

### 6.6 Running large sweeps without a chain script

iter-4's 4 sweeps and iter-5's 9 sweeps were both chained via
ad-hoc bash scripts. When the chain hangs at sweep #3 in the
middle of the night, you lose a day. **Standardize a chain
script that uses `bash run_g34_scaling_sweep.sh` per cell with
a watchdog and `# finished=` marker check.**

### 6.7 Not refreshing the ceiling claim after a tooling improvement

After iter-5 M1 corrected the ceiling, the iter-4 summary still
says "0.82 of ceiling" because it was committed under the wrong
ceiling. **Do not retroactively edit committed iter summaries**
(they are historical), but **do** add a "see iter-5 M1" footnote
the first time someone reads the iter-4 summary post-iter-5.

### 6.8 Stopping early because "result is obvious"

iter-2A: Phase 1 finished at 06:27 with deadline 11:59 (5h 31min
left). Phases 2–8 descoped. Two layered failures:

1. **Time-judgment used as stop-trigger** (now banned by §1.5).
   The 5h 31min of unspent deadline could have run the planned
   80-cell sweep (3-4 h chained) plus added receiver-side
   instrumentation. Both fit.
2. **Hand-calculated diagnosis substituted for measurement**.
   The "single-receiver fan-out is the new bottleneck" claim
   used to justify the descope had no receiver-side
   instrumentation backing it. The "1.5-2 µs per entry" number
   was derived by adding component costs (CXL load + DRAM atomic
   + flush + ack publish), never measured. So the descope
   rationale itself was a working hypothesis dressed as fact.

The right behaviour: when intermediate result is surprising or
disappointing, default to "run more measurement, not less."
Spare deadline → more verification, not less work. **§1.5 now
encodes this as a non-negotiable.**

If you find yourself thinking "the result is obvious / not worth
testbed time" — that's the exact moment to verify it with
instrumentation. The "obvious" diagnosis is the one most likely
to be wrong (it's the one you didn't measure).

---

## 7. Document trail per iter

Every iter produces 6 artifacts. None is optional.

| # | Artifact | Path | Size |
|---|----------|------|------|
| 1 | task_plan | `docs/iters/task_plan_<date>_<topic>.md` | 200–400 lines |
| 2 | latency_decomp (Phase 2) | `docs/iters/latency_decomp_<topic>_<date>.md` | 100–200 lines |
| 3 | sweep deliverable | `docs/sweeps/g34_scaling_ycsb_*_<ts>/` (or legacy flat path) | dir (15+ files) |
| 4 | iter summary | `docs/iters/iter<N>_<topic>_summary_<date>.md` | 100–200 lines |
| 5 | progress.md tail section | append `## <date> — iter <N> — <topic>` | 30–50 lines |
| 6 | runs_index row | append 1 row | 1 row |
| 7 | memory digest | `~/.claude/.../memory/project_<topic>.md` | 5–30 lines |

(Numbered 1–7 but #6 + #7 always travel together with #4 — call
it "5 deliverables + 2 index updates".)

---

## 8. Triggers for stopping a focus topic

When to declare "this protocol / subsystem is done for now and
move to the next." Apply at the end of any iter that meets one
of:

### 8.1 Bar passed on all primary workloads

Trivial case. Document the still-open work as "outstanding for
production-readiness" (see C retrospective § "Outstanding work")
and switch focus.

### 8.2 Bar passed on most + remaining bottleneck is structural

C iter-5 case: B and F pass, A misses by 3 % at 19.35 vs 20.
Diagnosis revised to "hot-bucket producer serialisation"
unaddressable by current protocol levers without a structural
redesign (slot-shard, LRC slack on hot buckets) that competes
with **opening Protocol A optimisation** for opportunity cost.
**Either focus would advance the project; pick the higher-ROI one.**

### 8.3 Layer-2 ceiling reached on the binding workload

If the binding workload's Layer-3 utilisation is > 0.85, the
remaining gap to the bar is bandwidth-only. The next move is
either byte-reduction (value-cache, increment update) or
layer-mode change (system-ram dax). Both are large engineering
investments; declare the protocol "BW-bound" and consider whether
those investments are worth a sub-project of their own.

### 8.4 Diminishing per-iter returns

If the last 2 iters each delivered < 10 % gain on the binding
workload, you are likely past the easy wins. Switch focus to a
fresher subsystem with more headroom; the original subsystem can
be revisited when a new technique arrives.

---

## 9. Recurring design patterns

When the same architectural shape appears across multiple
protocols / subsystems, it is a **pattern** worth naming. Below is
the running list. Add to it whenever a new pattern is observed
across ≥ 2 independent contexts.

### 9.1 "Aggregate-before-CXL"

**Statement**: Any cross-host communication topology with
N producers × M consumers should aggregate to host-level
endpoints in local DRAM **before** crossing CXL, so the CXL link
carries only host-to-host messages, not client-to-client.

```
                 Cross-host CXL
                 (H × H mesh)
   ┌─────────────────────────────┐
   │       (small)               │
   │                             │
   │  hostA ◀─────▶ hostB ◀─────▶ hostC ...
   │    ↑              ↑              ↑
   │    │              │              │
   │  ┌─────┐       ┌─────┐       ┌─────┐
   │  │MPSC │       │MPSC │       │MPSC │  ← per-host aggregator
   │  └─┬┬┬─┘       └─┬┬┬─┘       └─┬┬┬─┘    (DRAM, fast)
   │    ↑↑↑           ↑↑↑           ↑↑↑
   │   N clients     N clients     N clients
   │   (intra-host                      (large)
   │    DRAM only)
   └─────────────────────────────┘
```

**Why it works**: CXL latency (604 ns) and BW per host
(12.5 GB/s) are **scarce** relative to DRAM (174 ns / 393 GB/s).
N² traffic on CXL is bandwidth-bound at large N. Moving the N²
to local DRAM and reducing CXL traffic to H² (with H << N) keeps
the hot-path on the cheap layer.

**Applied instances** (3 confirmed, in chronological order):

1. **Protocol C iter-5 multi-flusher V2** (`src/cxl_batch_ring.{h,cc}`):
   N producer clients enqueue to per-flusher MPSC dirty queue (DRAM),
   flusher (per-host aggregator) does single bucket flush + bump_epoch
   on CXL. N producers → 1 (per-bucket-shard) flusher → 1 CXL atomic.
   720 runs 0 fails, design proven correct.

2. **Protocol A iter-2A wire (REVERTED)** (`docs/iters/iter2A_summary_20260426.md`):
   Half-aggregated form: N producers directly contend on shared MPSC
   ring tail; receiver still serial-pushes N-1 DramInvalQueue. Two
   bugs: (a) silently degraded strict A → LRC by publishing ack_seq
   before local clients consumed inval; (b) implemented half-aggregated
   arch instead of true N:1:1:N. **The negative example**: aggregating
   only one side of the path (producer side) without scaling the
   other (consumer fan-out) just moves the bottleneck. **Lesson
   added to corollary below.**

3. **Protocol A iter-2A-revised** (`docs/iters/iter2A_revised_summary_20260427.md`):
   True N:1:1:N: N worker threads → DRAM MPSC `LocalAggregatorQueue`
   → 1 sender thread per host (CXL-bound) → SPSC ring →
   1 receiver thread per host → atomic_store-via-coherence
   invalidation into shared DRAM `cache_epoch_arr`. Strict A
   linearizability designed-in via writer release-store + sender→
   receiver→sender ACK + receiver release-store. Code complete in
   tree (commit `[iter2A-rev-arch]`); empirical validation deferred
   to iter-3A pending testbed kernel restoration (uintr 6.15 lacks
   CONFIG_CXL_MEM).

### 9.1.1 Corollaries (added 2026-04-27 from iter-2A wire failure + iter-2A-revised design)

**Corollary 1 — Multi-consumer fan-out is not optional**: When N
producers per host > 1, the consumer side of the aggregated CXL
hop must also scale to N (or some K). Otherwise aggregation just
moves the bottleneck from CXL bytes-on-wire to CPU-bound receiver
fan-out. **Design fan-out for N consumers from day one.**
The iter-2A wire is the cautionary example: producer-side
aggregation without consumer-side scaling produced a 28× regression
at T=4 because the single receiver became the new serial chokepoint.

**Corollary 2 — Use x86 cache coherence for intra-host
invalidation fan-out, not explicit DramInvalQueue pushes**: A
single `std::atomic<uint64_t>::store(NEW, release)` to a
shared-memory cacheline (DRAM, MAP_SHARED across forked
processes) becomes immediately visible to **all N other local
clients** on their next acquire-load. ~5 ns. Compare to the
DramInvalQueue alternative: N-1 explicit MPSC pushes, each ~50–100 ns
amortised + an outer producer-side ACK gate per push. **Atomic_store
via coherence kills the receiver-side fan-out cost from O(N × per-push)
to O(1).** Designed in iter-2A-revised; awaiting empirical validation
once testbed is back.

**Corollary 3 — Sub-cacheline sharing is forbidden across CXL**:
Coherence-less inter-host CXL means a producer on host A
clflushopt-ing a sub-cacheline write back invalidates the entire
64-B line on host B's read. If two producers' entries share a
cacheline, the second producer's flush can clobber the first's
in-flight payload (false-sharing torn-write). **Every cross-host
ring entry must own its full 64-B cacheline**: PendingRingEntry
2-cacheline split (2026-04-22), iter-2A `PerHostOutEntry` 32→64 B
fix (`5c83965`), and iter-2A-revised `PerHostInvalEntry` all
encode this rule. Solution-2-style payload compression (16 B
per entry) cannot violate the alignment; if you want 4 entries
per cacheline, you also need a "this is the only writer of this
cacheline" claim flag — different design entirely.

**Building blocks already available**:
- `src/cxl_batch_ring.{h,cc}` MPSC ring code (iter-5 V2; correct).
  Reusable for the "outgoing aggregator" of Solution 1.
- `src/cxl_same_host_queue.{h,cc}` `DramInvalQueue` (Phase 4).
  Reusable for the "intra-host dispatch from replicator".
- `tests/cxl_dualhost_bw_bench.cc` (M1) measures the cross-host
  CXL link's actual ceiling; use to check whether the aggregated
  H² traffic stays comfortably under the ceiling.

**Anti-pattern (avoid)**: Allocating N² CXL-resident structures
because "the protocol model says every client should talk to
every client". The protocol model is a logical view; the physical
implementation should always check whether the N² fits in CXL
budget, and if not, aggregate to host level. Phase 4 same-host
bypass already half-applies this pattern (collapses the same-host
half of N²); Solution 1 closes the loop on the cross-host half.

**When to apply**: any time you see "N clients × M peers" pattern
where N > 4 OR M > 4 AND the path crosses CXL.

**Variants at larger H**: When num_hosts ≥ 8, even the H × H
cross-host mesh becomes a meaningful CXL load. At that point a
**3rd level** (gossip / tree replication across hosts) is worth
considering. With H = 2 the 2-level pattern is already optimal;
do not over-engineer the H = 2 case for a future H = 8 problem.

---

## Appendix — Reading order for a new contributor

When someone new (human or Claude) joins a new optimization push:

1. `CLAUDE.md` (root) — project rules
2. `docs/design_goals.md` — bar + 3-layer ceiling
3. `docs/scaling_ycsb_spec.md` — sweep procedure
4. **This document** (`docs/refs/optimization_methodology.md`)
5. The retrospective for the most recently completed focus topic:
   `docs/iters/protocol_c_retrospective.md`
6. The active iter's `task_plan_*.md`
7. memory `MEMORY.md` index (cross-conversation context)

Steps 1–3 are auto-loaded by tooling; steps 4–7 are read on demand.
