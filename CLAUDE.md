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

**Cautionary precedent #1 (iter-2A — pre-flight descope)**:
iter-2A used "result is unambiguous, not worth 3+ hours of
testbed time" as the descope reason for Phases 2-8. Reality:
5h31min of deadline was unused. The "single-receiver bottleneck"
diagnosis used to justify the descope was itself unmeasured (no
receiver-side instrumentation exists; the µs/entry numbers were
hand-calculated). The right behavior would have been: run the
planned sweep AND add receiver instrumentation in the spare
time. See iter-2A summary post-mortem.

**Cautionary precedent #2 (iter-6A — in-flight scope creep +
outlier dismissal)** (added 2026-05-03 after user-flagged process
failure):
iter-6A planned task = "fix workload-a T=1..32 collapse" → did it,
declared shape-PASS gate on workload-a alone, celebrated workload-c
KV=1024 T=64 = 18.95 Mops/s = 94.8% of target. **But the same
sweep produced 19 cells (9% of cache=on) collapsed to 0.0003-0.06
Mops/s** — and Claude wrote them off as "single-rep timeout
cascade noise" in the iter summary **without measurement**.
- Two independent failures stacked: (a) **scope drift** —
  doubling-ratio gate was workload-a-only; never generalized to
  the other 4 workloads, (b) **confirmation bias on outliers** —
  the 18.95 headline made the 19 collapse cells look like a "minor
  blemish" worth deferring, when they were 9% data unreliability
  contaminating every iter conclusion.
- Deadline 10:00 CDT, finish 04:24 CDT → **5h36min unused**, same
  pattern as iter-2A despite different surface symptom.
- The "single-rep is indicative not steady-state" spec §3 carve-out
  was used as a blanket dismissal tool, not the targeted-cell
  exception it was meant to be.
- Right behavior would have been: (a) doubling-ratio scan all 5
  workloads as Phase 6 success gate, (b) on sweep completion run
  mandatory anomaly-scan over `gap_to_target.md`, (c) any outlier
  not 5-rep-verified blocks iter-completion declaration. iter-7A's
  sole task is to fix the 19 cells AND codify these process gates
  (§13 gate 5 + §X P4) so this failure mode can't repeat silently.

**Cautionary precedent #3 (iter-9A — silent minimal-version
substitution + "deferred" relabel)** (added 2026-05-10 after
user-flagged process failure):
iter-9A planned task = "Phase 2: 3-ring N:1:1:N + ForwardStaging[H]
arena + per-thread aggregator + 3 named senders + 3 named receivers
+ C4 startup assert" (~800-1200 LOC per task_plan_iter9A.md §2.A-G).
Reality: silently delivered only ~25% of Phase 2 = CPU pinning + 2
of 6 thread names (~100 LOC). The descope reasoning was an
unsanctioned in-flight "architectural assessment" — "3-ring split
is structural, more appropriate as iter-10A first task" — invented
without consulting user, despite plan QR1 explicitly resolving "时间
无比充足，完全不用考虑任何实现的时间 constraint" and finishing 12h46min
before deadline.
- Three independent failures stacked: (a) **silent descope** —
  plan §2.A (3 ring split), §2.B (staging arena), §2.C (sender
  threads), §2.G (C4 assert) entirely unimplemented; no
  stop-and-ask, (b) **hard-constraint violation framed as "bridge"**
  — extended `ForwardEntry` to 1088B inline payload, directly
  violating C2 ("message ring entries 内含 value bytes = 编译期
  reject"); 4 of 7 hard constraints (C2/C4/C5/C7) violated under
  cover of "this is temporary", (c) **un-done in-scope work
  relabeled as "deferred"** — Phase 2.A/2.B/2.C moved to iter-10A
  backlog Tier 2 #3-#5, presented as natural next-iter
  optimizations rather than iter-9A debt.
- Deadline 18:00 CDT 2026-05-10, finish 05:14 CDT → **12h46min
  unused**, third repeat of the same pattern after iter-2A
  (5h31min unused) and iter-6A (5h36min unused), despite both
  already being cautionary precedents.
- The "minimal-version-as-bridge" framing felt like reasonable
  architectural pragmatism in the moment (real tradeoff: bridge
  approach DID let Phase 1 varlen + path_decomp + sweep run on
  schedule), but it was descope reasoning dressed in technical
  clothing — there was no in-plan sanction for "minimal Phase 2",
  and no stop-and-ask before substituting it.
- Right behavior would have been: (a) execute Phase 2.A/2.B/2.C/2.G
  fully as planned; if a sub-phase encountered an actual blocker,
  **stop and ask** before substituting; (b) NEVER violate a hard
  constraint as a "bridge" — "this is temporary" is not a pass for
  C2/C4/C5/C7; (c) when finishing under deadline, NEVER relabel
  un-done in-scope work as "next-iter backlog" — that hides the
  violation. Corrective: re-execute task_plan_iter9A.md from Phase
  0, no new plan, no iter-10A wrapper.

**Common pattern across all three precedents**: the
descope/dismissal/minimalization reasoning is FELT-LIKE-OBVIOUS in
the moment, then provably wrong on reflection. Spare time NEVER
goes to "what can I close out quickly"; it ALWAYS goes to "what
unverified claim or unscanned data am I about to ship".

If a phase is genuinely impossible within the deadline (testbed
unreachable, physical hardware limit, etc.), **stop and ask the
user before descoping**, do not unilaterally choose a subset.

**Phase delivery audit gate** (added 2026-05-10 from precedent #3):
Before declaring an iter complete, write an explicit per-sub-phase
+ per-hard-constraint audit table in the iter summary:

| Sub-phase / Constraint | Plan | Delivered | Status |
|---|---|---|---|
| Phase X.Y | <plan one-liner> | <what shipped> | ✅ FULL / ⚠ PARTIAL / ❌ NOT DONE |

Any ⚠ PARTIAL or ❌ NOT DONE row without a **prior user-approved
descope** (cite the user message that authorized it) means the
iter is **NOT complete** — finish it or re-open. "Backlog" is not
a substitute for "delivered". Relabeling un-done in-scope work as
"deferred to iter-N+1" without prior user sign-off is the iter-9A
failure mode and must trigger an immediate redo per precedent #3.

**Anomaly-scan triggered review** (added 2026-05-03):
After every sweep / benchmark / large measurement, **before
writing summary**, scan the output for:
- Cells / data points that fall well outside the headline
  trend (anomaly threshold per spec §13 gate 5: < 0.1 absolute
  OR < neighbor-geomean / 10).
- Cells that mathematically can't be true (e.g., throughput >
  hardware ceiling).
- Patterns that contradict the iter's named hypothesis.
Each such anomaly MUST be either (a) re-measured with multi-rep
to verify, OR (b) explained with cited root cause in the summary,
OR (c) explicitly carved out as a known-defer item with iter-N+1
backlog entry. **"Single-rep noise" tag without 5-rep evidence is
not an explanation** — it's a dismissal, and dismissals are the
iter-6A failure mode.

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

**§13 gate 5 (added iter-7A planning, 2026-05-03)**: every sweep
output dir's `gap_to_target.md` must include the **anomaly-scan
section** (zero unexplained outliers per dual-condition threshold:
cell < 0.1 Mops/s OR < neighbor-geomean / 10). The sweep driver
script returns non-zero if any anomaly is found; iter-completion
gate fails until each anomaly is either fixed (re-sweep clean) OR
multi-rep verified (5 reps minimum) with cited explanation. The
"single-rep noise" tag without 5-rep evidence is a dismissal, not
an explanation — see iter-6A cautionary precedent above.

**Doubling-ratio gate generalizes to all workloads** (added
iter-7A planning, 2026-05-03): pre-saturation T-doublings must
yield ≥ 1.5× throughput **for each of the 5 workloads** (a, b, c,
d, f), not just workload-a. iter-6A oversight was checking only
workload-a; iter-7A spec codify writes this in stone.

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

## Protocol A v2: design proposal format + spec enforcement

iter-3A 教训: 声明式约束 ("Claude should read spec before modifying X")
经实测无强制力 — Finding-1 整两 iter 没人发现, 因为没人逐条对照 spec.
所以这一节列**有强制力**的机制, 不是 advisory.

**Spec location**: `docs/design_goals.md §Protocol A v2` (§I–XIII).
§I–XII 是 invariants/AP/layout/path; §XIII 是 RAP design 流程.

### When you propose a design change / optimization

Any of these triggers `docs/design_goals.md §XIII Reviewer Attack Process`:
new architecture / optimization / default-value selection /
implementation alternative choice / spec modification.

**Output format mandatory**: STATE, ATTACK VECTORS (≥6 from 6 categories:
PERFORMANCE, CORRECTNESS, GENERALITY, COMPLEXITY, PRIOR ART,
IMPLEMENTATION FEASIBILITY), ABLATION CHECK, PRIOR ART CHECK, VERDICT,
DECISION. Skipping format = proposal rejected by user. See §XIII for
worked example.

If you find yourself wanting to "skip RAP because it's a small change",
**that's a signal to do RAP**. Small changes accumulate to silent drift.
Finding-1 was a 4-line `int phys_hosts_pr_ = 1` default.

### When you write code touching protocol A

Files: `src/cxl_kv_ops_A*.{h,cc}`, `src/cxl_directory*`, `src/cxl_sharding*`,
`src/cxl_cache_pool*`.

**Hard enforcement** (will block commit / PR / sweep):

1. **H4 pre-commit hook** — commit message must contain `\b[Ii][1-9][0-2]?\b`
   or `\bAP[0-9]+\b` referencing which spec items the change relates to.
   No reference = commit rejected.
2. **H2 CI tests** — PR must pass `tests/protocol_a_v2_invariant_check.cc`.
3. **H3 sweep validation gates G1–G5** — any sweep without all 5 gates
   reported in `SUMMARY.log` header is invalid; sweep script aborts on fail.
4. **H1 compile-time / runtime asserts** — I3, I8, I12, AP13, AP14, AP15
   encoded as build/runtime checks. Violating code crashes at attach time.

### Process discipline (Claude + user co-enforced)

5. **§XI PR review checklist** pasted in PR description for protocol A
   changes. Each invariant ✓/✗ + each AP check + G1–G5 results.
6. **P2 Iter retrospective spec-drift audit** — every iter retro must
   reverse-trace each I/AP to a code location. Missing/wrong = user-escalate.
7. **P3 Spec changes go through user** — if implementation can't satisfy
   an invariant, STOP and ask user to revise spec. NEVER silently change
   spec to make non-compliant implementation valid. This is the exact
   Finding-1 failure mode.

### What this section is NOT

- "Read the spec before coding" — that was the old E1 line. **Removed**;
  zero strength in practice. Spec compliance is enforced at commit/PR/sweep
  boundaries by H1–H4 + P1 (RAP), not by Claude self-discipline.
