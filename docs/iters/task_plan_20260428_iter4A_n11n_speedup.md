# Task plan — iter-4A — speed up cross-host invalidation (N:1:1:N true path)

**Author**: Claude (drafted at end of iter-3A)
**Date drafted**: 2026-04-28
**Status**: DRAFT — for user review at start of iter-4A
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter4A-profile]`, `[iter4A-ackbuf]`, `[iter4A-batch]`,
`[iter4A-sweep]`)

---

## Why this iter

iter-3A discovered that the Protocol-A N:1:1:N cross-host
invalidation path has been a **runtime no-op since iter-2A-revised**
(`phys_hosts_pr_` and friends never assigned in `attach()`; writer's
cross-host enqueue loop iterates 0 times). All iter-2A-rev / iter-3A
sweep numbers reflect "publish_slot writes to shared CXL +
worker_local cache_epoch_arr.store" — peers see new bytes via direct
CXL load, not via invalidation traffic.

iter-3A added the `FUSEE_ACTIVATE_N11N=1` opt-in. With it set:

- **Hash-diff: 3/3 PASS** with explicit pre-run cleanup (correctness OK).
- **Throughput**: workload A T=4 cache=on 0.97 Mops/s (vs 1.22 in
  no-op default → 19 % regression).
- **Tail behaviour**: T=1, T=2, T=8 with 200k ops time out at 600 s.
  T=4 happens to fit because K=2 channels match host count.
- **K-channel scaling under N:1:1:N true**: K=2 gives +42 % over K=1
  at T=4 — the K-channel design is structurally right; the absolute
  level is the issue.

Every figure of merit in iter-3A would be **simultaneously higher
and more meaningful** if N:1:1:N ran end-to-end. iter-4A's job is
to make that the case without paying the current 19-90 % overhead.

---

## Open Qs (please decide before Phase 1)

| # | Question | Default |
|---|----------|---------|
| Q1 | Should iter-4A also re-run the iter-3A sweep1+sweep2 with N:1:1:N true once the speedup lands, replacing the iter-3A "no-op" headline numbers? | **Yes** — the reframing-once-data-is-real is methodologically required (§6.5 attribution). |
| Q2 | Acceptable absolute regression vs no-op default at the end of iter-4A? | Aim for **≤ 5 %** regression on workload A T=4 cache=on (= 1.22 Mops/s × 0.95 = 1.16 Mops/s). |
| Q3 | Profile granularity: hardware perf counters (rdpmc) vs wall-clock probes? | **Wall-clock** to match Phase-6 instrumentation; perf counters as a separate sub-iter if wall-clock is inconclusive. |
| Q4 | Is dropping strict A acceptable for "fast invalidation" mode? | **No** — strict A is the iter-3A correctness witness; iter-4A must preserve it. |
| Q5 | Does iter-4A also fix `cxl_latency_decomp_A` to run with PER_HOST_RING=1 (Finding-2)? | **Optional** — could be a free side-effect of Phase 1, but not gating. |

---

## Phased breakdown

### Phase 1 — Profile the current N:1:1:N round-trip (`[iter4A-profile]`)

- Add cycle-counter probes around each step of dispatch_and_wait
  in the active branch (per_host_rings_enabled_ + aggregator_ + cache_epoch_arr_):
  - P1a: aggregator enqueue
  - P1b: writer ack-spin start → ack_op_id flipped
  - P1c: sender drain → ring publish
  - P1d: sender ack-channel poll → seq advance
  - P1e: sender writes worker_ack_buf cacheline
- Sweep workload A T={1, 2, 4, 8} cache=on with N:1:1:N true; emit
  per-stage µs and identify the dominant step.
- **Verify**: smoke runs at T=4 successfully complete; per-stage probe
  numbers sum to ≥ 90 % of measured wall-clock per op.

### Phase 2 — Co-locate worker_ack_buf on the worker's L1 (`[iter4A-ackbuf]`)

Hypothesis from iter-3A summary: ack-spin granularity on the
worker_ack_buf cacheline is the bottleneck. Currently each worker's
slot is in `aggregator_->ack_bufs[k][slot]` — physically allocated
in the host primary's pre-fork mmap, far from the worker's L1.

- Move worker_ack_buf to per-worker shm region allocated **after**
  fork (so each child's shm sits on the same NUMA / L1 as the worker
  thread that polls it).
- Update sender_loop_k to write into the per-worker buffer via a
  pointer table (`std::vector<WorkerAckSlot *> per_worker_ack`).
- Verify: hash-diff still PASS; T=4 cache=on Mops/s improves.

### Phase 3 — Tighter batch-timeout adaptation (`[iter4A-batch]`)

iter-3A measured FUSEE_SENDER_BATCH_T_US in {2, 5, 10, 20, 100} and
found 2-20 µs gives ~1 Mops/s; 100 µs collapses to 0.11. So the
default 20 µs is fine, but the sender's `did_work` polling loop
itself burns CPU even when there's nothing to drain.

- Replace `nanosleep(1µs)` with adaptive backoff: nanosleep(N) where
  N ramps 1µs → 10µs → 100µs while idle, resets to 1 µs on first work.
- Verify: idle CPU usage drops; T=4 throughput unchanged or improves.

### Phase 4 — Re-run iter-3A sweeps with N:1:1:N true (`[iter4A-sweep]`)

- sweep1' (K=1, per-slot LFM, FUSEE_ACTIVATE_N11N=1, T_top=84): 80 cells
- sweep2' (K=2, per-slot LFM, FUSEE_ACTIVATE_N11N=1, T_top=82): 80 cells
- 30-plot Style B deck for each per `scaling_ycsb_spec.md`.
- Comparison plot vs iter-3A no-op sweeps (same axes).

### Phase 5 — Phase-6-equivalent decomp re-run with N:1:1:N true

- Same T grid, same workload A, but with Phase-1's new probes
  enabled. 5-stage breakdown should show **where** the speedup landed.

### Phase 6 — iter-4A summary + index updates

- `docs/iters/iter4A_summary_<date>.md`
- Methodology refresh: §9.1.1 — iter-4A-corrected K-channel data.
  iter-3A's "K=2 +42 %" claim under no-op is now superseded by the
  iter-4A real-path number.

---

## Out of scope

- B-protocol same N:1:1:N rewire (deferred to iter-5A).
- Variable-KV value-size for A (iter-4 work, port to A still pending).
- Read-path cache=on collapse at high T (separate iter).
- Hot-bucket fan-out for workload A (deferred to iter-5A; orthogonal
  to invalidation path).

---

## Success criteria

1. iter-3A's Finding-1 closed: N:1:1:N runs end-to-end with no opt-in env.
2. Workload A T=4 cache=on N:1:1:N regression ≤ 5 % vs no-op default.
3. 80/80 OK on each of sweep1' and sweep2'; hash-diff still 100 % PASS.
4. Phase-1 probe data identifies the dominant stage and shows it
   reduced post-Phase-2 / Phase-3.
5. iter-4A summary cites every Mops/s number with stage attribution.

---

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Phase 2 ack-buf relocation breaks the sender's per-worker write path (now requires shared-memory pointer table) | Med | Phase 1 hash-diff is the gate; if hash-diff regresses, revert to current ack-buf layout and try a different speedup. |
| Phase 1 probe overhead distorts measurements | Low | rdtsc is ~30-cycle overhead; per-op ~3000 cycles → 1 % distortion. Compare LATENCY_DECOMP=0 vs 1 smoke. |
| New ack-buf layout requires runtime config changes incompatible with iter-3A's checkpoint | Low | iter-4A is not constrained by iter-3A binary compatibility; old binaries always re-attach via attach(). |
| Profile finds bottleneck is in the CXL coherence-less ack-channel publish flow → no software fix | Med | Document the finding; pivot to alternative invalidation strategy (e.g., piggyback invalidations on CXL data write traffic). |
