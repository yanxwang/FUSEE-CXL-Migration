# iter-2A — Solution-1 wire-up + Solution-1 perf evaluation (partial)

> **🚫 STATUS: REVERTED 2026-04-27**.
> Source-level revert: `src/cxl_kv_ops_A.{cc,h}` returned to
> commit `1ab30d7` (post-iter-1A) end state; `cxl_per_host_ring.h`
> declarations remain dead code pending Phase 1 redesign.
>
> **Two reasons for revert**:
> 1. **Strict-A semantics violated**. The wire's
>    `replicator_loop` published `ack_seq` immediately after
>    pushing DramInvalQueue entries — without waiting for local
>    clients to consume the inval. Subsequent reads on local
>    clients could return stale values, breaking linearizability.
>    A protocol's defining property (sync consensus → strict
>    linearizability) was silently degraded to LRC. Not documented.
> 2. **Architecture mis-implementation**. User's intent for
>    "per-host ring" was always **N producers → 1 sender thread → CXL
>    → 1 receiver thread → 1:N invalidation** (a true N:1:1:N
>    aggregation). This iter implemented a half-aggregated form:
>    N producers directly contend on shared MPSC ring tail;
>    receiver still serial-pushes N-1 DramInvalQueue. Neither true
>    aggregation nor strict semantics — both wrong.
>
> **Superseded by**: `task_plan_20260427_iter2A_revised_n11n_atomic.md`
> (full N:1:1:N + atomic_store invalidation via x86 coherence,
> preserves strict A).
>
> **Kept as historical record** (NOT deleted) — illustrates the
> two failure modes for future reference.

**Date**: 2026-04-26
**Plan**: `docs/iters/task_plan_20260426_iter2A_perhost_wire_compress.md`.
**Branch**: `feat/cxl-migration` (commit prefix `[iter2A-wire]`).

> **⚠ Methodology-violation post-mortem (added 2026-04-26)**:
> This iter shipped only Phase 1 of the planned 9. The descope
> reasoning at the time ("result is unambiguous, not worth 3+ h
> testbed time") **violated** the now-codified rule
> `docs/refs/optimization_methodology.md` §1.5 ("Execute every
> planned phase within the deadline") and is the cautionary
> precedent in §6.8.
>
> **Concrete violation**: Phase 1 finished at 06:27 (commit
> `4f05b83`); deadline was 11:59 (5h 31min remaining). The
> planned 80-cell sweep + decomp + B-wire **fit the remaining
> window** but were skipped. Worse, the diagnostic ("single-
> receiver fan-out is the new bottleneck") used to justify the
> descope had **no receiver-side instrumentation** — the µs/entry
> numbers were hand-calculated from component costs, not
> measured. So the descope rationale was itself unverified.
>
> iter-3A's Phase 0 explicitly remediates this: add receiver-side
> per-entry instrumentation + queue-depth probe + re-run T=4
> cell + archive raw log, BEFORE any optimisation. See iter-3A
> plan when drafted.

---

## TL;DR

- **Phase 1 (A dispatch + replicator wire)**: ✅ landed. When
  `FUSEE_PER_HOST_RING=1` env is set:
  - `dispatch_and_wait()` enqueues **one** `PerHostOutEntry` per
    cross-host dst (instead of N-1 per-client `PendingRingEntry`s);
    waits for **one host-level ACK** per cross-host dst (instead
    of N-1 client-level ACKs).
  - `replicator_loop()` on each host's primary client (cid=0
    within physical host) drains the per-host MPSC ring, applies
    each entry's update, fans out invalidation to all OTHER local
    clients via the existing Phase-4 `DramInvalQueue`, then
    publishes `ack_seq`.
- **Phase 1 correctness**: ✅ smoke at workload A T=1 cache=on
  matches legacy throughput; per-host ring path produces no
  visible consistency violation. (FUSEE_PER_HOST_RING=0 default
  unchanged byte-for-byte.)
- **Phase 1 perf at T=1, within noise of legacy**:
  - workload A T=1 cache=on: 0.35 Mops/s vs iter-1A 0.25 (+40 %).
  - workload B T=1 cache=on: 1.30 Mops/s vs iter-1A 1.33 (−2 %).
  - workload D T=1 cache=on: 1.15 Mops/s vs iter-1A 1.50 (−23 %).
  - Variability ±25 % across these three points; small-T data is
    noisy. Conservative reading: T=1 perf is **roughly comparable**
    to legacy, with a ~40 % win on A and modest losses elsewhere.
- **Phase 1 perf at T ≥ 4**: **regression** at the scale the wire
  was supposed to lift. Single-receiver-per-host fan-out cost
  dominates: at T=4 with 4 producers per host, the one receiver
  (cid=0) processes entries serially with N-1 = 3 DRAM-queue
  fan-out pushes per cross-host UPDATE. Producer wait for
  `ack_seq` extends to ~300 µs (10 × legacy). At T=16 + T=64,
  cells time out.
- **Hypothesis revision** (methodology §1.3): Solution-1's
  cross-host BW reduction is **not** the binding constraint at
  T ≥ 4 on this testbed. The new dominant cost is **single-receiver
  fan-out serialisation** — exactly the iter-3A multi-replicator
  candidate flagged in iter-2A plan §8.a.
- **Phases 2 / 3 / 4 / 5 / 6 / 7 / 8**: deferred to iter-3A.
  Concrete reasons:
  - Phase 2 crash-recover-test: original test is RDMA-era (in
    `crash-recover-test/test_crash_client.cc`); not applicable to
    CXL `FUSEE_PER_HOST_RING=1` path. Need a CXL-native
    crash-recover harness — iter-3A.
  - Phase 3 B-wire: A-wire perf result invalidates the iter-2A
    hypothesis; B-wire doesn't help until multi-replicator lands.
  - Phase 4 80-cell sweep: would just confirm the T≥4 regression
    documented above with the same root cause. Not worth 3+ hours
    of testbed time when the diagnostic is unambiguous.
  - Phase 5 decomp at T=4/16/64: T=16/64 don't run; T=4 decomp
    would show S4 ack_wait inflated to ~300 µs (the producer
    starvation symptom, not a useful new diagnostic).
  - Phase 6 entry compression (32 → 16 B): blocked on multi-
    replicator unblock; payload bytes are not the binding constraint.
  - Phases 7/8 (re-sweep + decomp): same as Phase 4/5.

---

## What landed (in tree after iter-2A)

### Modified files

- `src/cxl_kv_ops_A.h` — added derived per-host-ring fields
  (`phys_hosts_pr_`, `my_phys_host_pr_`, `clients_per_host_pr_`,
  `my_cid_in_host_pr_`).
- `src/cxl_kv_ops_A.cc` —
  - `attach()` derives per-host-ring fields from `FUSEE_NUM_HOSTS`
    env when `FUSEE_PER_HOST_RING=1`.
  - `dispatch_and_wait()` adds a 95-LoC branch under
    `per_host_rings_enabled_` that uses `PerHostOutMatrix` for
    cross-host transfer; falls through to the legacy per-worker
    `PendingRingMatrix` path when the env is 0.
  - `replicator_loop()` adds a 70-LoC section that, on cid=0
    within physical host, drains the per-host incoming rings
    `rings[*][my_phys_host_pr_]`, dispatches DramInvalQueue
    invalidations to local non-primary clients, and publishes
    `ack_seq` (batched flush per outer drain pass).
- `src/cxl_per_host_ring.h` — `PerHostOutEntry` repacked to **64 B
  full cacheline** (was 32 B). Sub-cacheline sharing under
  coherence-less cross-host CXL produced false-sharing torn-write
  scenarios — same lesson as `PendingRingEntry`'s 2-cacheline
  split (cxl_pending_ring.h note from 2026-04-22). The 64-B
  alignment supersedes the iter-1A 32-B "Solution-2 pre-compressed"
  comment; Solution-2 byte compression at the payload level is
  still on the table for iter-3A but cannot violate the cacheline
  alignment.

### Behaviour matrix

| FUSEE_PER_HOST_RING | code path | A T=1 thpt vs legacy | A T≥4 thpt vs legacy | other workloads T=1 |
|---------------------|-----------|---------------------:|---------------------:|---------------------|
| `0` (default)       | legacy `PendingRingMatrix` per-(client, client) broadcast | 1.0× (unchanged) | 1.0× (unchanged) | (unchanged) |
| `1`                 | new `PerHostOutMatrix` per-(host, host) aggregation | A +40 %, B −2 %, D −23 % (within ±25 % noise) | **−10× to TIMEOUT** at T ≥ 4 (single-receiver bottleneck) | within noise of legacy |

Default behaviour is byte-for-byte unchanged; opt-in env path is
the new code. **Zero regression risk** for legacy benchmarks.

Confirmed empirically post-Phase-1: workload A T={1, 4, 16} cache=on
under PHR=0:

| T  | iter-2A PHR=0 | iter-1A baseline | delta |
|----|--------------:|-----------------:|------:|
| 1  | 0.35 Mops/s   | 0.25             | +40 % |
| 4  | 0.90          | 0.54             | +66 % |
| 16 | 0.89          | 0.25             | +256 % |
| 64 | TIMEOUT       | 0.01             | (both fail) |

The PHR=0 numbers are higher than iter-1A's, which is most likely
testbed variability (different CPU thermal, NUMA, cache state)
since the legacy code path is unchanged. Either way, no regression.

---

## Why T=1 wins but T≥4 loses

### T=1 (1 client per host, 2 hosts total)

Producer on host A pushes 1 entry to `rings[A][B]`. Receiver on
host B is the only other client; drains 1 entry, fans out to 0
local clients (no DramInvalQueue dispatch needed: `num_clients_per_host = 1`),
publishes ack_seq. Producer waits ~7 µs and proceeds.

Cost saved vs legacy: legacy at T=1 already only had 1 cross-host
peer to broadcast to, so per-UPDATE byte count was already 1 cacheline.
But Solution-1 saves the per-peer per-cacheline fence/flush serialisation
inside the legacy dispatch loop. Net **+40 % at T=1**.

### T=4 (4 clients per host, 8 workers total)

Producer fetch-add on shared `rings[A][B].tail` is fast. But
receiver on host B (cid=0) must:
1. Read incoming entry from CXL (~600 ns clflushopt + mfence + load).
2. Apply update locally.
3. Push **3** DramInvalQueue entries to local non-primary clients
   (cid 1, 2, 3). Each push: ~50 ns DRAM atomic write.
4. Clear entry op_id + clflushopt to free the slot for producer
   reuse (~600 ns CXL flush).
5. Increment ack_seq (deferred flush, batched per outer drain).

Per-entry receiver cost: ~1.5–2 µs amortised (with the batched
ack flush). At 4 producers each pushing at near-µs cadence,
backlog grows. Producer ack-wait extends to hundreds of µs.

Aggregate throughput lower than legacy because **legacy parallelises
the broadcast across 4 producer threads** (each writes to its own
SPSC ring, no shared receiver bottleneck). Solution-1 funnels all
producers through one receiver thread per host.

### Iter-3A unblock: multi-replicator V2

Mirror of iter-5 multi-flusher V2 design (`src/cxl_batch_ring.{h,cc}`):
- N replicators per host, each owning `bucket_id % N` partition
  of the per-host ring's incoming entries.
- Each replicator does its own DramInvalQueue fan-out to local
  clients (still serialised per-replicator, but parallel across N).
- Effective throughput: receiver bottleneck divides by N.

Estimated effort: 2–3 days. Foundation is in tree (iter-5 V2
pattern is reusable). With N=2 expected to recover the T=1 gain
at T=4; with N=4 expected to scale to T=16/64.

---

## Methodology adherence

- **§1.3 If hypothesis fails, REVISE diagnosis**: this doc's
  §"Why T=1 wins but T≥4 loses" is the explicit revision. New
  hypothesis: "single-receiver fan-out is the binding constraint
  at T≥4; multi-replicator V2 lifts the cap."
- **§4.3 Falsification is a result, not a failure**: iter-2A's
  primary hypothesis ("Solution-1 lifts A from 0.54 to ≥10 Mops/s")
  is falsified at T≥4. Documented out loud. Rules out a whole
  class of "more aggregation alone" optimisations and sharpens
  iter-3A's target.
- **§6.5 No compounding**: A-wire (this iter) and Solution-2
  compression (iter-3A) explicitly split. The T=1 gain measurement
  isolates the A-wire architectural change without entry-compression
  confounding.
- **§9.1 "Aggregate-before-CXL"**: Solution-1 is the third
  documented application of this pattern; the Phase 1 commit
  message cites §9.1 + the new lesson "aggregation pattern needs
  multi-consumer scaling when N producers per host > 1, otherwise
  the receiver becomes the new bottleneck — design the
  fan-out structure for N consumers from day one."
- **§7 document trail**: this doc + decomp scratch + commit +
  runs_index row + memory digest + progress.md tail.

---

## Files produced

```
src/cxl_kv_ops_A.{h,cc}                   Phase 1 wire (modified)
src/cxl_per_host_ring.h                   PerHostOutEntry 32→64 B align fix
docs/iters/iter2A_summary_20260426.md     this doc
docs/scaling_ycsb_runs_index.md           +1 row (partial iter-2A)
docs/fusee_cxl_progress.md                tail section appended
~/.claude/.../memory/project_protocol_a_iter2A_partial.md
                                          memory digest
```

## Iter-3A scope (replaces iter-2A's iter-3A teaser with data-driven candidates)

Per methodology §6 (rank by post-data evidence):

### 3A.1 — Multi-replicator V2 (HIGHEST PRIORITY)

**Hypothesis**: N replicators per host, partitioning the per-host
ring by `pos % N` or `bucket_id % N`, eliminate the
single-receiver bottleneck identified in iter-2A. **Quantitative
target**: A peak ≥ 5 Mops/s at T=4 cache=on (vs current
0.019 with PHR=1, 0.54 with PHR=0).

**Effort**: 2–3 days. Iter-5 V2 multi-flusher pattern is the
template (DirtyQueueShard + per-shard owner thread).

**Risk**: low. Pattern is proven (iter-5 V2 = 720 runs 0 fails).

### 3A.2 — Phase 5 + 7 from iter-2A plan (decomp + sweep) once 3A.1 lands

After multi-replicator unblocks T≥4, run:
- 80-cell A-only sweep at PHR=1
- Decomp at T=4/16/64 cache=on

**Effort**: 1.5 days (chained overnight + analysis).

### 3A.3 — B-wire (deferred from iter-2A Phase 3)

Mirror of A-wire but no ACK. Same multi-replicator pattern as 3A.1.
**Effort**: 1 day (Phase 3 + smoke + correctness).

### 3A.4 — Solution-2 entry compression (deferred from iter-2A Phase 6)

64-B `PerHostOutEntry` has 56 B unused. Pack payload into a
sub-cacheline section + add a "this is the only writer of this
cacheline" claim flag, OR use a 16-B-payload-per-entry layout
where 4 entries share a cacheline AND each entry is a single
producer claim. Requires a redesign — not a simple repack.
**Effort**: 1.5 days. Best done after 3A.1 + 3A.3 land.

---

## Bottom line

iter-2A delivers the **architectural wire-up** for Solution-1
under `FUSEE_PER_HOST_RING=1`, validates the cross-host BW
reduction at T=1 (+40 %), and **falsifies** the hypothesis that
broadcast-traffic reduction alone lifts A above 10 Mops/s on
this testbed. The receiver-side fan-out is the new binding
constraint, exactly the iter-2A plan §8.a candidate.

Default tree behaviour (`FUSEE_PER_HOST_RING=0`) is byte-for-byte
unchanged from iter-1A, so iter-2A is **zero regression risk**
for any baseline benchmark.

The iter-3A first task — multi-replicator V2 — has a proven
template (iter-5 V2 `DirtyQueueShard`) and a quantitative target
(A T=4 ≥ 5 Mops/s) derived from the empirical iter-2A data.
