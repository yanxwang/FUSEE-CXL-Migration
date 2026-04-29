# FUSEE-CXL migration — project rules for Claude

## Iter execution discipline (HIGHEST CONSTRAINT — read first)

**Within a user-given deadline, execute every planned phase.** Do
NOT use time judgment to descope, defer, or skip planned work
just because remaining time looks short or because intermediate
results suggest the next phase is "predictable / not worth the
testbed time." The user's planning unit is **hours** (typical
window: 7–10 h). Plans estimated in days fit in those hours
empirically; assume they will fit unless physically impossible.

Default behavior:
- Push through every planned phase to completion.
- If a measurement / sweep / experiment is in the plan, **run it
  and archive the raw data**. "The result will be obvious" is
  never a valid reason to skip — measurement IS the deliverable.
- Spare time goes to **MORE verification** (extra cells, extra
  decomp instrumentation, controlled-comparison reruns), never
  to dropping planned work.

Acceptable extensions of scope (use proactive judgment HERE):
- Modest deadline overruns of 30–60 min to finish a planned
  measurement.
- Adding an unplanned experiment that strengthens or falsifies a
  hypothesis (e.g., adding receiver-side instrumentation when the
  decomp suggests a new dominant stage).
- Re-running a flaky cell to confirm a number.

Unacceptable behaviors (these are violations, NOT judgment calls):
- Stopping early because "result is obvious."
- Deferring planned phases to next iter without first attempting
  them within the deadline.
- Using a partial smoke test to substitute for a planned full
  sweep.
- Treating a hypothesis-falsification as license to skip
  remaining planned work — falsification is a result, not a
  stop-trigger; the planned sweep + decomp still ship and become
  the data that informs the next iter.

**Cautionary precedent**: iter-2A used "result is unambiguous,
not worth 3+ hours of testbed time" as the descope reason for
Phases 2-8. Reality: 5h31min of deadline was unused. The
"single-receiver bottleneck" diagnosis used to justify the
descope was itself unmeasured (no receiver-side instrumentation
exists; the µs/entry numbers were hand-calculated). The right
behavior would have been: run the planned sweep AND add receiver
instrumentation in the spare time. See iter-2A summary post-mortem.

If a phase is genuinely impossible within the deadline (testbed
unreachable, physical hardware limit, etc.), **stop and ask the
user before descoping**, do not unilaterally choose a subset.

**Deadline semantics** (added 2026-04-27 after user
clarification):
- Deadline = "fully push the planned phases + autonomously add
  verification experiments as data arises." Empirically, every
  user-given deadline has been **comfortably sufficient** for
  the day-scale plans they cover.
- Modest overrun (≤ ~1.5 h) is acceptable IF the time is spent
  on (a) finishing a planned phase, or (b) running an
  unplanned-but-warranted experiment to verify a hypothesis.
- Spare time priority: **more verification > additional
  controlled-comparison cells > re-running noisy cells >
  documentation polish**. Spare time NEVER goes to "deciding
  what else to descope."

**Time estimates in plans are forbidden** (added 2026-04-27):
- Do NOT include hour/day estimates in any plan deliverable
  (no "Total: ~X h", no per-phase "X h" columns, no buffer
  numbers).
- Estimates have repeatedly biased descope decisions
  (iter-2A precedent: "10 d plan in 5 h window" psychologically
  triggered the descope; the actual work fit comfortably).
- If sub-phase ordering or risk attribution is genuinely needed,
  use **qualitative** language ("Phase 5 has the most code
  changes → highest debug risk"; "this iter exceeds typical
  scope; consider split?"), never quantitative.
- Internal mental ranking of phase effort is fine; do not
  surface it in any user-visible doc.

---

## North-star goal (read `docs/design_goals.md` first)

The CXL migration is **not complete** until both of these hold on
the g3 + g4 testbed:

- YCSB-C (100 % read)     sustains **≥ 20 Mops/s aggregate**
- YCSB-A (R50 U50 Zipf)   sustains **≥ 20 Mops/s aggregate**

A 3–5× improvement over the previous sweep is a checkpoint, not a
stopping condition. Every performance result must be compared to
the 20 Mops/s bar, and the remaining gap must be analyzed (latency
decomposition, identification of dominant stage) before declaring
a phase done. See `docs/design_goals.md` §"Analysis discipline".

## Canonical benchmark procedure

For any `scaling_ycsb` experiment, read `docs/scaling_ycsb_spec.md`
first. Scope, output layout, plot set, and reproducibility
requirements are spec-bound. Don't silently shrink scope when a
test times out — ask the user or raise timeout, then record the
choice in the summary doc.

## Phase plan reference

Overall throughput-improvement plan is in
`docs/refs/ABC_throughput_improvement_plan.md`. Progress doc is
`docs/fusee_cxl_progress.md`.

## Code organization

- Three CXL protocols live side-by-side: `src/cxl_kv_ops_{A,B,C}.cc`,
  all implementing the `CxlKvStore` public surface. Selection is
  compile-time via `-DCONSENSUS_OPT=FUSEE_OPT_{A,B,C}`.
- Shared primitives: `src/cxl_hashtable.h`, `src/cxl_bucket_lock.h`
  (LFM wrapper), `src/cxl_pending_ring.h`, `src/cxl_same_host_queue.h`,
  `src/cxl_oplog.h`, `src/cxl_mm.{h,cc}`.
- LFM mutex primitives come from the sibling repo
  `~/cxl_shm_profiling/` (included via CMake).
- Tests: `tests/cxl_ycsb_runner.cc` (YCSB), `tests/cxl_kv_bench*.cc`
  (micro-bench), `crash-recover-test/` (recovery).

## Hosts

- `g3`, `g4`: dual 86-core Intel Xeon nodes sharing a CXL Type-3
  memory expander via PCIe switch. Device `/dev/dax0.0`, 512 GiB
  devdax. Kernel 6.15.0 after the PXE rebuild.
- `~/FUSEE_CXL/` on each host is the sync target; source lives on
  the workstation under `/home/yanwang/FUSEE/` and is rsync'd over.
- Rebuild after sync: `cd ~/FUSEE_CXL/build-cxl && make -j16 <targets>`.

## Commit hygiene

- Single branch: `feat/cxl-migration`.
- Commit prefixes: `[phase]`, `[2a]`, `[2b]`, etc. matching the
  plan item.
- Do not push to remote without explicit request.

## Protocol A v2 spec compliance (iter-4A onward — HIGHEST CONSTRAINT for protocol A work)

`docs/design_goals.md` §Protocol A v2 (sections I–XII) is the binding
spec for any iter-4A or later change to protocol A. Failures to follow
it caused iter-3A's silent design drift (Finding-1 routing field
default, single-CXL-copy越界). Applies equally tight as §"Iter
execution discipline".

**Rules**:

1. Before modifying any of `src/cxl_kv_ops_A*.{h,cc}`, `src/cxl_directory*`,
   `src/cxl_sharding*`, `src/cxl_cache_pool*` — **read `docs/design_goals.md`
   §Protocol A v2 in full first**. Reference invariant numbers (I1-I12)
   or anti-pattern numbers (AP1-AP15) in commit messages.

2. **PR review checklist** (`docs/design_goals.md §XI`) must be pasted
   into commit description for any PR touching protocol A. Each
   invariant ✓/✗ + each anti-pattern check + validation gate results.

3. **Spec changes go through user**. If implementation can't satisfy
   an invariant, STOP and ask user to revise spec. **Never silently
   change spec to match a non-compliant implementation** (this is the
   exact failure mode that produced Finding-1).

4. Validation gates G1–G5 (`docs/design_goals.md §IX/X`) must run on
   every iter-4A sweep; results captured in `SUMMARY.log` header.
