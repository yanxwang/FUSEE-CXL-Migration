# Task plan — protocol C write-path optimization (deadline 2026-04-23 10:00 CDT)

## Objective

Push **protocol C** above **20 Mops/s aggregate trans_agg_thpt on
workloads A, B, F** on the g3+g4 testbed. This is a direct step
toward the migration's north-star goal (`docs/design_goals.md`).

Protocols A and B are **not in scope** for this task; A/B numbers
under the same optimizations are a stretch goal at the end, not a
validation requirement.

## Baseline (from `scaling_sweep_p2_v4_20260422_205644`, cache=on)

Protocol C `trans_agg_thpt` at T=32:

| workload | Mops/s | gap to 20 Mops/s |
|---|---|---|
| workloada (R50 U50) | 0.56 | **36×** |
| workloadb (R95 U05) | 2.53 | 8× |
| workloadf (R50 RMW) | 0.80 | 25× |

Evidence the gap is not hardware-bound: C on workloadd (INSERT)
stays at w_p99 ≈ 10–15 µs through T=64. The blow-up on workloads
A/B/F is Zipfian hot-bucket contention on the LFM lock.

## Iteration loop

Each iteration runs **steps 1 → 2 → 3**. If step 3 does not meet the
20 Mops/s bar on A, B, and F, loop back to step 1 with fresh data;
**do not mechanically repeat the same optimization**.

### Step 1 — Latency decomposition

Instrument the C write hot path (`insert`, `update`) with stage
timestamps:

```
t0 = enter insert/update
t1 = lock_table_.lock() returns
t2 = 7-slot bucket scan complete
t3 = publish_slot returns
t4 = bump_epoch returns
t5 = lock_table_.unlock() returns
```

Report per-stage `avg / p50 / p99` in µs for each of `T ∈ {8, 16, 32, 64}`
on workload A, collected from both g3 and g4.

**Implementation**: new micro-bench `tests/cxl_latency_decomp_C.cc`
(fork-based, same client model as `cxl_ycsb_runner`, per-client rdtscp
histograms, merged by the primary client before printing).

**Artifact**: `docs/latency_decomp_C_<iterN>_<ts>.md` with per-stage
tables and a conclusion identifying the dominant stage.

### Step 2 — Targeted optimizations

Selection is driven by step-1 data, not pre-committed. Default order:

**2.1 Enable ticket-lock (scaffolded, zero code change)**
- `cmake -DFUSEE_USE_TICKET_LOCK=ON ..` and rebuild on both hosts.
- Re-run step-1 decomposition.
- Expected: if LFM O(MAX_HOST_NUM=200) scan was the dominant cost,
  `t1 - t0` drops meaningfully.
- Decision: keep the flag on if it helps; move to 2.2 regardless.

**2.2 Per-slot lock**
- Change: `BucketLockEntry` becomes an array of 7 per-slot entries
  (one per `kCxlKvSlotsPerBucket` slot). Each entry is a ticket-lock
  body (~16 B), NOT a full LFM block.
- Writer flow: scan the 7 slots to find the target (empty for INSERT,
  matching key for UPDATE/DELETE) without holding any lock, then
  acquire only that slot's ticket-lock, do the write, release.
- Code:
  - `src/cxl_bucket_lock.h/.cc` — add `SlotLockTable` variant.
  - `src/cxl_kv_ops_C.cc` — call `lock_slot(bucket, slot_idx)` /
    `unlock_slot(bucket, slot_idx)` instead of the current per-bucket
    API.
- Memory cost: ~7 MB for the 65 536-bucket table (acceptable).
- Correctness preserved: same semantics as current per-bucket lock
  (serialize writers targeting the same slot); readers still
  seqlock-retry on `write_epoch` which remains per-bucket.
- Re-run step-1 decomposition.

Further steps (2.3+) are **not pre-committed** — they are chosen
after a new decomp if 2.1 + 2.2 do not meet the 20 Mops/s bar.

### Step 3 — C-only scaling validation

Run the scaling sweep restricted to protocol C:

- `OPTS="C"`
- `WORKLOADS="workloada workloadb workloadc workloadd workloadf"`
  (all five kept; regression-check that C's read and INSERT paths
  do not degrade)
- `THREADS="1 2 4 8 16 32 64 86"`
- `CACHE_MODES="on off"`
- Total runs: 1 × 5 × 8 × 2 = **80**

Output directory: `docs/g34_scaling_ycsb_C_only_<yyyymmdd_HHMMSS>/`.
Per-iteration layout (minimal):

- `SUMMARY.log`
- `plot_commit.txt` (git SHA + date + runner env)
- `C_thpt_workload{a,b,c,d,f}.png` (5 plots)
- `C_lat_workload{a,b,d,f}_write.png` + `C_lat_workload{b,c,d,f}_read.png`
- `iteration_note.md` — one paragraph stating which decomp + optimization
  produced this run.

**Pass condition** (for loop exit):
- `trans_agg_thpt ≥ 20 × 10^6` on C at some T for **each** of
  workloada, workloadb, workloadf.

If met → go to §"Finalization".
If not met → back to step 1 with the new numbers.

## Finalization (after target met)

1. The terminal iteration's sweep also generates the spec-compliant
   extras for C: a "C-only" `extra/` directory with `C_compare_<wl>.png`
   throughput and write-p99 overlays across iterations (showing the
   progression from Phase 2 baseline → each optimization step → final).
2. Update `docs/scaling_ycsb_runs_index.md` with one row per iteration.
3. Append `docs/fusee_cxl_progress.md` with a note on what landed and
   the final numbers.
4. **Stretch**: run a full A/B/C spec-compliant scaling sweep (240
   runs per spec or 220 with `A_SKIP_AT=64`) to check whether the
   per-slot lock helps or hurts protocols A and B. No validation bar
   on A/B for this task; data point only. Skip if <1 hour left before
   deadline.

## Time budget (indicative only, do not self-report stop)

| phase | est. wall clock |
|---|---|
| Step 1 build + first decomp | 1.5 h |
| Step 2.1 ticket-lock enable + re-decomp | 0.5 h |
| Step 2.2 per-slot lock impl + re-decomp | 2.5 h |
| Step 3 first C-only sweep | 0.5 h |
| Iteration 2 (if needed) | 2–3 h |
| Finalization + stretch | 1 h |

Total 8–10 h; start time ~21:30 2026-04-22, deadline 10:00 2026-04-23
= 12.5 h window.

Per user instruction: **do not stop to report** at intermediate
milestones; carry through to deadline. Course-correct based on data,
not clock.

## Ground rules restated

- Follow `docs/scaling_ycsb_spec.md` for output layout (C-only variant).
- Every reported number compares against 20 Mops/s explicitly; no
  "3–5× over baseline" celebrations (`docs/design_goals.md` §"Analysis
  discipline").
- Cross-host: rsync `src/` to both `g3` and `g4`, rebuild
  `~/FUSEE_CXL/build-cxl/` on both, then run via role-mode scripts.
- Commits land on `feat/cxl-migration`. Prefix tags: `[decomp]`,
  `[ticket-lock]`, `[per-slot-lock]`, `[C-sweep]`.
