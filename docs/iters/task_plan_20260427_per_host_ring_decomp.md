# Experiment plan — per-host-ring sender + receiver decomposition

**Author**: Claude
**Date drafted**: 2026-04-27
**Status**: DRAFT v1 — awaiting user QA on Q-X1 and Q-X2 below
**Branch**: `feat/cxl-migration` (commit prefixes `[ph-decomp-instr]`,
`[ph-decomp-run]`, `[ph-decomp-analysis]`)
**Type**: standalone diagnostic experiment (NOT an iter). Output
data informs iter-3A design but this plan does not commit to any
iter-3A direction.

---

## Open Qs (please decide before Phase 1)

| # | Question | Default | User position |
|---|----------|---------|---------------|
| Q-X1 | Reps per cell — 1 / 2 / 3 runs to control for noise? | **2 reps** per cell. 8 cells × 2 = 16 runs total ≈ 30-40 min. iter-2A's biggest weakness was unrepeated single-shot measurements; 2 reps lets us see noise floor. 3 would be safer but adds 50% wall-clock. | ⏳ |
| Q-X2 | Should this experiment also implement the **alternative architecture** (your Q1 design — N:1 sender thread + 1:N replicator + bidirectional ACK aggregation)? Adds ~150 LoC; lets us compare half-aggregated (current iter-2A wire) vs full-aggregated (your design). | **No, not in this experiment**. Adds one more variable to a measurement already isolating 6 hypotheses. iter-3A picks direction based on this experiment's output. The current experiment validates which bottleneck is binding for the iter-2A code-as-shipped, which is enough to inform iter-3A. | ⏳ |
| Q-X3 | User-given deadline + autonomy mode | undecided; estimate **~9 h** wall-clock | ⏳ |

---

## 1. Motivation (why this experiment exists)

iter-2A shipped Solution-1 wire (commit `4f05b83`) and concluded
"single-receiver fan-out is the new bottleneck at T≥4" — but the
diagnosis had **zero receiver-side instrumentation backing it**.
The "1.5-2 µs per entry" receiver cost was hand-calculated by
adding component costs, never measured. iter-2A's PHR=1 raw log
was not even archived.

This experiment is the missing measurement layer. Output:
empirical answers to "which of 6 candidate bottleneck hypotheses
is binding for workload A under PHR=1 at T ∈ {2,4,8,16}."

iter-3A direction (multi-replicator V2 vs true sender aggregation
vs other) is decided **after** this experiment, by the data.

## 2. Hypothesis space

6 candidate bottlenecks, each with a distinguishable experimental
signature:

| H | Bottleneck | Signature in data |
|---|---|---|
| H1 | Single receiver thread can't keep up with N producers | receiver per-entry stage time roughly flat with T; queue depth grows with T; producer S5'(wait_ack) inflates linearly with T |
| H2 | MPSC tail.fetch_add contention among N producers | sender S3'(claim_ring_slot) latency grows roughly O(N²) with T (atomic CAS contention); same on receiver side flat |
| H3 | Cross-host CXL ACK roundtrip (1.2 µs hard floor) is the floor | producer S5'(wait_ack) has a flat ≈ 1.2 µs minimum even at T=2; multiplicative inflation with T attributable to backlog × 1.2 µs |
| H4 | Cacheline ping-pong on ring slot reuse | sender S3'(spin for op_id==0) grows with T; receiver R5(clear_slot) flat |
| H5 | Receiver does N-1 serial DramInvalQueue pushes per UPDATE | receiver R4(dram_inval_dispatch) grows linearly with T (~50 ns × (N-1)) |
| H6 | Initialization / measurement-window noise | bottleneck signature shifts when ops/cell raised from 200K to 500K; or stage variance is huge |

Each H is **independent** in the sense that the experiment can
classify each as **confirmed / rejected / insufficient-data**
based on the empirical signature.

## 3. Experimental design

### 3.1 Cell matrix

Single workload, single cache mode, 4 thread counts, 2 PHR modes:

| dimension | values | size |
|---|---|---|
| workload | A only | 1 |
| T (clients per host) | 2, 4, 8, 16 | 4 |
| cache mode | on only (validation experiment; cache=off out of scope per user CQ2) | 1 |
| PHR mode | 0 (legacy), 1 (per-host ring) | 2 |
| ops per cell | 200K (per user CQ1; default) | — |
| reps per cell | 2 (per default Q-X1) | 2 |

**Total: 4 × 2 × 2 = 16 runs.** PHR=0 is the controlled-baseline
sibling at the same wall-clock window so PHR=0 vs PHR=1 comparison
is environment-noise-free (iter-2A's gap).

### 3.2 Sender instrumentation (PHR=1 path)

iter-1A's S1-S5 instrumentation is on the **legacy** path. PHR=1
path needs its own probes. Add to `src/cxl_kv_ops_A.cc` under
`per_host_rings_enabled_` branch, gated by `FUSEE_LATENCY_DECOMP=1`:

| stage | covers | when |
|---|---|---|
| S1' lock_acquire | LFM bucket lock | covered by existing S1 (shared) |
| S2' local_apply | 7-slot scan + slot write + flush | covered by existing S2 (shared) |
| S3' claim_ring_slot | `r->tail.fetch_add` + `flush_line(&tail)` + sfence | NEW |
| S4' wait_slot_free | spin loop on `e->op_id == 0` (incl. flush+full_fence per attempt + count of attempts) | NEW |
| S5' write_entry | memcpy entry fields + `e->op_id = op_id` + `flush_line(e)` | NEW |
| S6' wait_ack | spin on `r->ack_seq` advancing past my_ph_pos[dst_host] | NEW |
| S7' unlock + epoch | bump_epoch + LFM unlock | covered by existing S5 |

Per-thread accumulator (avoid contention on global counters).

### 3.3 Receiver instrumentation (PHR=1 path)

Add to `replicator_loop()` `per_host_rings_enabled_` branch:

| stage | covers |
|---|---|
| R1 wait_for_entry | spin from "no entry" to "entry observable" (clflushopt + load until op_id != 0) |
| R2 read_from_cxl | clflushopt + mfence + load full entry payload after op_id != 0 |
| R3 apply_local | bucket scan + slot write + flush bucket cacheline |
| R4 dram_inval_dispatch | loop pushing N-1 DramInvalEntry to local DramInvalQueues |
| R5 clear_slot | `e->op_id = 0` + `flush_line(e)` |
| R6 publish_ack_seq | `r->ack_seq.store + flush_line` (note: batched per drain pass) |

Single-thread (cid=0 per host); accumulator local. Plus per-pass
**count of entries drained** — for receiver throughput rate.

### 3.4 Queue-depth probe

Independent helper thread per receiver host:
- Every 100 ms, sample `r->tail.load() - r->head_local` for each
  `rings[*][my_host_]`. (head_local is replicator's local cursor.)
- Append (timestamp_ns, depth) tuple to per-cell `queue_depth.csv`.
- Stop on benchmark end signal.

This is **independent** of producer/consumer rate — gives
arrival-vs-service-rate visibility (Little's law cross-check).

### 3.5 Output per cell

```
logs/g34_phr_decomp_<ts>/
  <T>_<phr>_<rep>/
    g3.log                   # raw runner stdout (incl. SUMMARY line)
    g4.log
    sender_stages_g3.csv     # per-thread S3'/S4'/S5'/S6' p50/p99 + counts
    sender_stages_g4.csv
    receiver_stages_g3.csv   # R1-R6 per-pass cumulative + counts (cid=0 only)
    receiver_stages_g4.csv
    queue_depth_g3.csv       # 100ms samples
    queue_depth_g4.csv
  SUMMARY.log                # YCSB lines from each cell, all 16 runs
```

## 4. Phased breakdown

| # | Phase | Prefix | Deliverable | Est | Verify |
|---|-------|--------|-------------|----:|--------|
| 0 | Plan review | — | This doc + Q-X1/X2/X3 decided | — | user sign-off |
| 1 | Sender-side instrumentation | `[ph-decomp-instr]` | `src/cxl_kv_ops_A.cc` PHR=1 branch + new S3'-S6' probes; per-thread accumulator + `dump_sender_stages()` API | 2 h | builds; smoke at T=2 prints non-zero per-stage counts; `FUSEE_LATENCY_DECOMP=0` build byte-for-byte unchanged (cmp on stripped binary) |
| 2 | Receiver-side instrumentation | `[ph-decomp-instr]` | `replicator_loop()` PHR=1 branch + R1-R6 probes; single-thread accumulator + `dump_receiver_stages()` API | 1.5 h | smoke prints per-stage counts; sum of R1-R6 ≈ replicator wall-clock loop time within 5% |
| 3 | Queue-depth probe + benchmark integration | `[ph-decomp-instr]` | `tests/cxl_ycsb_runner.cc` wires probe thread when `FUSEE_QUEUE_DEPTH_PROBE=1`; dumps `.csv` files at end | 0.5 h | smoke at T=4 produces non-empty queue_depth.csv |
| 4 | Build + 1-cell smoke under both PHR modes | `[ph-decomp-instr]` | smoke run T=4 PHR=0 + T=4 PHR=1 each ~30s; verify all dump files appear and are sane | 0.5 h | every dump file non-empty + parseable |
| 5 | 16-run cell matrix | `[ph-decomp-run]` | chained script runs `T ∈ {2,4,8,16} × PHR ∈ {0,1} × 2 reps`; archive everything to `logs/g34_phr_decomp_<ts>/`; SUMMARY.log appended | 1.5 h | 16/16 runs OK; FAIL-cell pattern (if any) noted |
| 6 | Per-stage analysis | `[ph-decomp-analysis]` | per-cell sender stage table + receiver stage table + queue depth median/peak; rep-to-rep variance reported | 1 h | tables in summary doc + raw csv archived |
| 7 | Hypothesis adjudication | `[ph-decomp-analysis]` | each of H1-H6 classified (confirmed / rejected / insufficient-data) with the specific data column cited | 0.5 h | every H has a verdict + 1-line evidence |
| 8 | Style B plots per Sect 5 + scaling_ycsb_spec §7 | `[ph-decomp-analysis]` | (a) sender stages stacked bar vs T (PHR=0 + PHR=1 side by side), (b) receiver stages stacked bar vs T (PHR=1 only), (c) queue depth time-series at T=8/16 cache=on PHR=1, (d) producer wait p50/p99 vs T (PHR=0 vs PHR=1 line) | 1 h | 4 PNGs in `docs/iter2A_postmortem/` using `from plot_style import apply_style` |
| 9 | Summary doc + index updates | `[ph-decomp-analysis]` | `docs/iters/per_host_ring_decomp_summary_20260427.md` + progress.md tail + runs_index row + memory digest | 1 h | all 4 index updates landed |

**Total: ~9 h.** Buffer to 11 h for instrumentation debug or
unexpected receiver behaviour.

## 5. Per-Hypothesis adjudication rules (Phase 7 framework)

Each H gets one of {**Confirmed**, **Rejected**, **Insufficient**}:

### H1 — Single-receiver bottleneck

**Confirmed if** all of:
- Receiver R1+R2+R3+R4+R5+R6 sum at T=16 ≥ 80% of one wall-clock entry interval (receiver near 100% utilization)
- Queue depth median grows monotonically with T from T=2 to T=16
- Producer S6'(wait_ack) growth with T tracks queue_depth × per-entry-receiver-time (Little's law sanity)

**Rejected if** receiver utilization < 50% even at T=16.

### H2 — MPSC tail-fetch_add contention

**Confirmed if** sender S3'(claim_ring_slot) p50 grows worse than
linear with T (e.g. T doubling → S3' grows by 3-4 ×).

**Rejected if** S3' stays roughly flat across T (atomic doesn't
saturate at T ≤ 16).

### H3 — CXL ACK roundtrip floor

**Confirmed if** producer S6'(wait_ack) at T=2 is already
≥ 1.2 µs (the round-trip floor) AND scales like queue × 1.2 µs.

**Rejected if** S6' at T=2 < 1 µs (no floor visible at low T).

### H4 — Cacheline ping-pong on slot reuse

**Confirmed if** sender S4'(wait_slot_free) attempt count grows
significantly with T (more retries to find a free slot).

**Rejected if** S4' attempt count ≤ 2 at all T (slot almost
always free first try).

### H5 — Receiver N-1 serial DramInval push

**Confirmed if** receiver R4(dram_inval_dispatch) µs/entry grows
linearly with T (slope ≈ 50-100 ns per added local client).

**Rejected if** R4 stays flat with T (e.g. push is parallelised
or batched somewhere I missed).

### H6 — Init / measurement noise

**Confirmed if** rep-to-rep variance > 30% on key metrics
(receiver utilization, queue depth, producer wait_ack).
Implication: **redo experiment with longer ops/cell**.

**Rejected if** rep-to-rep within ±10%.

If 2+ Hs co-confirmed, classify them by relative impact (which one
contributes most µs to per-op latency).

## 6. Verification (per-phase gates)

### Phase 1+2+3 instrumentation correctness

- `cmp` stripped binary `FUSEE_LATENCY_DECOMP=0` build = byte-identical to current default build (instrumentation truly opt-in).
- Smoke at T=2 PHR=1: every stage's accumulator non-zero, per-thread (sender) and single-thread (receiver) counts match expected op count.
- Receiver stage sum ≈ wall-clock loop time within 5% (no major un-attributed time).

### Phase 4 smoke

- Both PHR modes produce SUMMARY line + dump files non-empty.
- Sender thread count = T (per host).
- Receiver thread count = 1 per host.
- queue_depth.csv has ≥ 50 samples (5 sec run × 100 ms).

### Phase 5 sweep

- 16/16 runs OK. FAIL must be archived with reason.
- Per-cell rep variance: if rep-1 and rep-2 differ > 30 % on agg_thpt, re-run rep-3 and use median.

### Phase 7 adjudication

- Every H has a verdict + 1-line citation of data column.
- If "insufficient" verdicts > 2, document explicitly what additional measurement would resolve and propose follow-up (do NOT pretend conclusive when not).

## 7. Success criteria

iter-2A-postmortem-decomp succeeds when ALL of:

1. All 16 runs complete; raw logs archived in
   `logs/g34_phr_decomp_<ts>/`.
2. 16 sender_stages_*.csv + 16 receiver_stages_*.csv + 16 (or 8 — only PHR=1) queue_depth_*.csv all parseable.
3. Phase 7 adjudication: every H1-H6 has confirmed/rejected/insufficient verdict + cited evidence.
4. Phase 8: 4 Style B plots produced, `from plot_style import apply_style` used.
5. Summary doc lists the **dominant bottleneck stage at T=16** with quantitative attribution (e.g. "receiver R4 = 78% of receiver per-entry cost at T=16, accounts for X µs of the Y µs producer wait").
6. Summary doc names the **2-3 candidate iter-3A directions** ranked by expected impact, **derived from the data** (not from intuition).
7. All index updates landed (progress.md tail + runs_index + memory digest).

**No throughput target.** This is a diagnostic experiment.
Throughput is the dependent variable being explained, not a goal.

## 8. Risk

| Risk | Likelihood | Mitigation |
|---|---|---|
| Instrumentation perturbs measurement (each `clock_gettime` ≈ 25 ns × 6 stages × 200K ops = 30 ms total ≈ 0.5% overhead — acceptable, but check) | Low | Compare `w_avg_ns` from PHR=1 + DECOMP=1 vs PHR=1 + DECOMP=0 at T=4; if delta > 5% accept caveat in summary |
| Receiver instrumentation single-thread overhead amplifies the bottleneck we're trying to measure | Med | Receiver is only loop body code; probes added inside the loop; conservative per-stage `clock_gettime` placement (start + end of stage, no nested probes). If overhead suspect, add a control run with R1+R6 only |
| Queue-depth probe thread contends with replicator on the same atomic | Low | Probe reads `r->tail` (atomic) + `r->head_local` (plain int owned by replicator); reading is non-blocking; sampler at 100ms cadence is negligible CPU |
| 16 runs hit a testbed instability not seen at smoke | Med | Phase 4 smoke covers exactly this; Phase 5 chained script logs each run's exit code; on FAIL, run continues, FAIL noted, retry once at end |
| 200K ops/cell at T=16 = ~12.5 K ops per worker — too short to clear init noise | Med-High | If H6 (noise) confirmed, re-run with 500K and report the 500K table as authoritative |
| **Methodology violation risk**: tempted to skip "obvious" cells | **N/A** — § "Methodology adherence" below makes this a hard rule | — |

## 9. Methodology adherence (post-iter-2A discipline)

Per `CLAUDE.md` §"Iter execution discipline" + `optimization_methodology.md` §1.5 / §6.8:

- **All 16 runs ship**, regardless of intermediate finding. If T=2 PHR=1 looks "fine" → still run all 4 reps × T=2/4/8/16. The data IS the deliverable.
- **Spare time** at end of window goes to: (a) re-running cells with high variance, (b) adding instrumentation that the data suggests is missing (e.g. if R4 dominates and we want sub-stage breakdown of "which cid push is slowest"), (c) controlled re-run of a noisy cell. **NOT** to writing more interpretation of partial data.
- **No "obvious" claims unmeasured**: every adjudication verdict cites a specific data column.
- **Raw logs always archived** under `logs/g34_phr_decomp_<ts>/` — iter-2A's "ad-hoc smoke not archived" mistake does not repeat.

## 10. What this experiment **does not do**

- Implement the alternative N:1 sender architecture (Q-X2 default = no, deferred to iter-3A).
- Touch protocol B (B's PHR=1 wire is iter-3A scope).
- Sweep cache=off or other workloads (validation experiment scope).
- Solution-2 entry compression (orthogonal; iter-3A scope).
- Decide iter-3A direction. Output **informs** iter-3A.

## 11. iter-3A teaser (DEFERRED — no commitment)

After this experiment lands, iter-3A picks from these candidates
based on **which H is confirmed dominant**:

- H1 confirmed → multi-replicator V2 (mirror iter-5 V2)
- H2 confirmed → introduce host-level sender thread (your Q1 design)
- H3 confirmed → batched / async ACK (one ACK per K UPDATEs)
- H4 confirmed → larger ring depth + slot-skip on busy slots
- H5 confirmed → multi-replicator with shard-by-cid (each replicator owns subset of local cids it dispatches to)
- H6 confirmed → re-do with 1M ops, this experiment was inconclusive

iter-3A direction selection is **data-driven** by Phase 7 output.
Plan goes after this experiment ships, not before.

---

## Appendix — methodology cross-reference

- `CLAUDE.md` §"Iter execution discipline" — every cell ships, no descope.
- `docs/refs/optimization_methodology.md` §1.2 diagnose before optimizing — this whole experiment IS the diagnose step before iter-3A optimize.
- §1.5 + §6.8 — explicit anti-pattern remediation for iter-2A.
- §3.2 latency decomp methodology — sender-side reuses iter-1A pattern; receiver-side is new application of same pattern.
- §4 hypothesis discipline — 6 named falsifiable hypotheses with adjudication rules upfront, not selected post-hoc.
- §7 document trail — Phase 9 deliverables.
- §9.1 Aggregate-before-CXL pattern — this experiment will inform the **corollary** (consumer-side scaling rule) with empirical data; methodology §9.1 will be updated post-experiment if data confirms the corollary.
- `docs/scaling_ycsb_spec.md` §7 — Style B plots mandatory; Phase 8 explicitly cites.
