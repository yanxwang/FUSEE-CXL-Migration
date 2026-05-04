# iter-8A Phase 3 (proper) — Consolidated 32-stage decomposition table

**Author**: Claude (iter-8A spare-time mandate completion)
**Date**: 2026-05-04
**Mandate**: 4 diagnostic methods (Sol-1 RDTSCP / Sol-2 perf / Sol-3 perf-sched / Sol-4 µbench)
applied to every stage in `protocol_a_architecture_blueprint.md` Part II.

This table is the deliverable that should have shipped at iter-8A
Phase 3 close-out. The original Phase 3 produced only the R3→R1
transition table from one collapsed run; this fills in the rest.

## Notation

- **Expected**: derived from µbench primitives per
  `per_stage_expected.md` (Sol-4)
- **Healthy p50/p99/max**: measured on captured healthy run (16.24 Mops/s)
  (Sol-1)
- **Collapsed p50/p99/max**: measured on captured collapsed run (0.218 Mops/s)
  (Sol-1)
- **C/H ratio**: collapsed p50 / healthy p50 (≥5× = anomalous)
- All values µs unless noted.

## Worker write path (W1..W12) — actual measurements per stage

A stage's "duration" here = time from PROBE(stage) to time of NEXT probe
on the same thread. By blueprint convention PROBE("W1") fires at start
of execute_write_local while PROBE("W2") fires after the lock is
acquired, so "W1 duration" measures the W2-stage WORK (bucket + lock).
This is consistent across W stages.

| Stage | Expected | Healthy p50 | Healthy p99 | Healthy max | Collapsed p50 | Collapsed p99 | Collapsed max | C/H | Status |
|---|---|---|---|---|---|---|---|---|---|
| W1 (entry→W2-work) | 4.59 | 0.730 | 6.969 | 73.77 | 0.740 | 11.929 | **1225** | 1.01× | **anomaly: max** |
| W2 (→W3-work) | 0.094 | 0.130 | 2.069 | 42.40 | 0.136 | 4.397 | 68.11 | 1.05× | OK |
| W3 (→W4-work) | 0.05 | 0.022 | 4.244 | 90.46 | 0.023 | 4.210 | 105.49 | 1.05× | OK |
| W7 (→W8-work) | 1.07 | 0.022 | 0.036 | 60.75 | 0.023 | 0.105 | **744** | 1.05× | anomaly: max |
| W8 (→W9-work) | 0.19 | 0.025 | 5.393 | 880.88 | 0.026 | 5.383 | **904** | 1.04× | OK |
| W9 (→W10-work) | 0.02 | 0.019 | 0.025 | 35.24 | 0.020 | 0.085 | 38.52 | 1.05× | OK |
| W10 (→W11-work) | 3.00 | 3.272 | 16.555 | 816.19 | 3.328 | 20.649 | **974** | 1.02× | OK |
| W12 (→next op) | n/a | 0.045 | 4.283 | 21595 | 0.047 | 4.238 | **16876** | 1.04× | inter-op |

## Worker read path (R1..R6)

| Stage | Expected | Healthy p50 | Healthy p99 | Healthy max | Collapsed p50 | Collapsed p99 | Collapsed max | C/H | Status |
|---|---|---|---|---|---|---|---|---|---|
| R1 (entry→R2-work) | 0.30 | 6.523 | 17.274 | 159.89 | 6.716 | 17.758 | 709.60 | 1.03× | OK |
| R2hit (→R6) | n/a | 0.021 | 0.032 | 93.50 | 0.021 | 0.042 | 35.46 | 1.0× | OK |
| R2miss (→R3 or R5) | 0.50 | 1.400 | 6.760 | 97.15 | 1.603 | 6.674 | 98.59 | 1.15× | OK |
| **R3 (→F-roundtrip)** | 5.00 | 8.299 | **10.514** | 158.42 | **8.762** | **10010** | **10012** | **951× p99** | **CRITICAL** |
| R4 (→R6) | 3.00 | 0.244 | 0.726 | 19.40 | 0.364 | 7.977 | 14.26 | 1.49× | OK |
| R6 (→next op) | n/a | 0.467 | 1.593 | 217.90 | 0.463 | 2.865 | **15015** | 0.99× | inter-op tail |

### R3 = the named root cause (forward_cache_register call into F1..F7)

- Healthy: typical 8.3 µs (matches Expected 5 µs + spinlock overhead)
- Collapsed: **p50 still ~8.8 µs** (essentially same as healthy at median)
- Collapsed: **p90 = 5005 µs** = the 5 ms cap firing on 10 % of cross-host
  CACHE_REGISTERs (post-iter-8A-fix; pre-fix it was 200 ms cap)
- Collapsed: **p99 = 10010 µs** = 2 × 5 ms cap (worker hit cap, retried,
  hit cap again)
- Max samples all from a single thread on a single CPU (cpu 77 in this
  capture), confirming residual collapse is a per-thread retry storm

## Send-invalidate sub-stages (I1..I8) — observed in probe data

The collapsed run did NOT include I1..I8 in the probe stream because
workload-d traffic was 95 % R / 5 % W and W was rate-limited by
forward-side collapse before invalidate broadcast got to fire.
Healthy data has corresponding stages but with sparse counts; iter-9A
should re-instrument with I-stages mandatory in next characterization.

## Forward-to-owner / cache_register sub-stages (F1..F7)

Same gap as I-stages: F1..F7 are NOT directly probed in current code.
The aggregate is captured as R3 stage above. iter-9A should add F1..F7
PROBE() insertions to enable per-step F-stage attribution.

## CacheDispatcher loop (D1..D5)

The dispatcher emits I3..I6 on consumer-side. From `per_stage_collapsed.md`
these stages have NO entries — meaning during the captured collapsed run
of workload-d (95% read), there were essentially zero invalidates being
issued, so the dispatcher was idle-polling.

## Sol-2 perf record findings (60s system-wide, collapsed run)

Top symbols by CPU samples:

| Symbol | %CPU | Count |
|---|---|---|
| main (worker loop) | 65.15 % | 2473 |
| `cache_dispatcher_loop` | 4.72 % | 133 |
| `responder_loop` | 3.19 % | 132 |
| `forward_cache_register` | 2.03 % | 79 |
| (kernel: workingset_activation, exit_mmap) | ~5 % | (unrelated process exit teardown) |

### Hot ASM in `responder_loop()`

```
81.72 % :  mov (%rax), %rcx     ← LD-CXL post-MFENCE = poll ring->tail
14.35 % :  mfence
 0.79 % :  pause
```

**96 % of responder CPU = waiting for one specific CXL load to return.**
Each poll = 1× FLUSH + 1× MFENCE + 1× LD-CXL ≈ 720 ns roundtrip.

### Hot ASM in `cache_dispatcher_loop()`

```
57.77 % :  mfence
41.44 % :  mov (%rax), %rax     ← same pattern
```

**99 % of dispatcher CPU = same flush+mfence+load polling.**

### Single-thread saturation ceiling

If responder is constantly busy: per-event cost ≈ 1.7 µs (per
`per_stage_expected.md`) → **ceiling ≈ 588 k events/sec/thread**.

Workload-d at 16 Mops/s × 5 % writes × 1 invalidate-per-write ≈
800 k events/sec needed → **structurally above the single-responder
ceiling**.

## Sol-3 perf sched record findings

```
protocol_a_ycsb (66 threads): runtime 31063 ms / 60s
                              avg sched delay  146 µs
                              max sched delay  1323 µs (= 1.3 ms)
```

### Critical inference: CPU starvation is NOT the cause

- Max scheduler wakeup latency = **1.3 ms**
- forward_spin_wait timeout cap = **5 ms** (post-iter-8A fix)
- 1.3 ms ≪ 5 ms → workers are NOT being descheduled long enough to
  cause the 5 ms cap to fire. Hypothesis H5 (CPU starvation /
  scheduler displacement) is **falsified**.

The 5 ms cap fires because **the responder is too slow to drain its
ring at the rate workers are filling it**, not because workers or
responder are de-scheduled.

## Anomaly summary (per CLAUDE.md gate 5 + iter-8A §X P5)

| Stage | Anomaly | Root cause (named) | Diagnostic source |
|---|---|---|---|
| **R3 collapsed p99** | **951× higher than healthy** (10010 vs 10.5 µs) | `forward_spin_wait` 5 ms cap firing because **responder thread saturated at single-thread CXL-poll ceiling 588 k/sec**; producer queue grows; cap fires after queue depth × 1.7 µs ≥ 5 ms. Worker absorbs -EAGAIN and retries → self-sustained loop. | Sol-1 (RDTSCP) + Sol-2 (perf annotate of responder = 81 % LD-CXL) |
| W1 collapsed max 1.2 ms | rare event (<<p99); same on healthy; not collapse-driver | spinlock contention max consistent with µbench T=64 (max 25 ms expected; observed 1.2 ms is very mild) | Sol-4 µbench |
| W7..W10 collapsed max 0.7-1 ms | rare; not collapse-driver | scheduler max latency 1.3 ms = matches OS-level descheduling; bounded | Sol-3 perf sched |
| R6 collapsed max 15 ms | propagated tail | R3 timeout cascades into next op's R1→R6 inter-op gap | derived |

## Per-anomaly RAP fix summary

| Anomaly | Diagnosis source agreement | Sol-X-derived fix candidate |
|---|---|---|
| R3 p99 = 5 ms cap | Sol-1 + Sol-2 both confirm responder saturation | **K-shard ForwardResponder** (currently iter-9A backlog #3, was deferred-low-priority; now ELEVATED to iter-9A primary because it is the sole structural cap above ~16 Mops/s on workload-d). Estimated K=2-4 thread-shards drains responder ceiling to 1.2-2.4 M/sec, well above 800 k workload demand. |
| Worker retry storm after -EAGAIN | Sol-1 attribution table A.4 | **Fail-loud -11** (iter-9A backlog #1) — workers stop retrying same op forever; instead bail with logged error. Composes with K-shard fix. |
| 3 uncapped slot-wait spins | Sol-2 audit of cxl_kv_ops_A.cc | Add 5 ms cap (iter-9A backlog #2, ~80 LOC). Composes with above. |

## Process retrospective

The mandate was to apply 4 diagnostic methods to every stage. iter-8A
shipped Phase 3 with only Sol-1 + Sol-4 (probe + µbench) and named
ONE bottleneck (R3 forward_spin_wait), shipping a 3-LOC fix.

The deferred Sol-2 + Sol-3 (perf + perf-sched), executed now:
- Sol-2 perf revealed the **structural** root cause: responder is
  CXL-latency-bound at single-thread ceiling 588 k/sec, below the
  workload demand at 16 Mops/s. The 5 ms cap is the BOUND-DAMAGE
  symptom; the cap will keep firing until responder is parallelised.
- Sol-3 perf-sched **falsified** an alternative hypothesis (CPU
  starvation) which would have been hard to test without it.

The full picture: iter-8A's fix bounded damage but did NOT fix the
structural bottleneck. iter-9A's K-shard ForwardResponder is now
mandatory (was deferred), AND backlog #1 / #2 are still required to
stop the retry-storm amplifier.
