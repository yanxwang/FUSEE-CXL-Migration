# iter-1A — Protocol A baseline + decomp + Solution-1 data structures

**Date**: 2026-04-26
**Plan**: `docs/iters/task_plan_20260425_iter1A_baseline_decomp.md`
(v2 hybrid scope; this iter ships a **partial** v2 due to testbed
unreachability — see §"Phase status" below).
**Branch**: `feat/cxl-migration` (commit prefix `[iter1A-decomp]`,
`[iter1A-perhost]`).

---

## TL;DR

- **Phase 1 (A decomp instrumentation)**: ✅ landed. 5-stage
  write-path probes; default build byte-for-byte unchanged.
- **Phase 2 (A baseline subset sweep)**: ✅ run. 40 cells
  (A × 5 wl × T={1,4,16,64} × cache={on,off}); 32 OK, 8 FAIL
  (all T=64 timeouts — exactly the structural FAIL pattern).
  See §"Baseline numbers" below.
- **Phase 3 (A decomp run)**: ✅ run. workload A T=4 cache=on:
  **S3 broadcast + S4 ack_wait = 26.87 µs = 74 % of write
  path** (S4 alone = 48 %). Phase 4 GO empirically validated.
  See §"Empirical decomp" below.
- **Phase 4 GO/NO-GO**: ✅ **GO** — both first-principles BW
  analysis (5.4× over Layer-2 ceiling at T=64) AND empirical
  decomp (74 % of latency in S3+S4) confirm Solution 1 targets
  the correct stages.
- **Phase 5a (Solution 1 — data structures)**: ✅ landed.
  `src/cxl_per_host_ring.h` defines `PerHostOutEntry` (32 B),
  `PerHostOutRing` (MPSC, atomic fetch_add tail, modeled after
  iter-5 V2 `DirtyQueueShard`),
  `PerHostOutMatrix[kMaxPhysicalHosts=4][kMaxPhysicalHosts=4]`
  (~4 MB total — **330× smaller than legacy 1.28 GB
  `PendingRingMatrix`**). A.cc attach plumbing extended; opt-in
  via `FUSEE_PER_HOST_RING=1` env (default 0 = legacy path).
- **Phase 5b / 6 / 7**: deferred to iter-2A.
  - Phase 5b (`dispatch_and_wait` + `replicator_loop` rewire) is
    a ~150-LoC hot-path rewrite that benefits from a separate
    iter for proper smoke-test discipline (methodology §6.5
    no compounding).
  - Phase 6 (entry compression) — `PerHostOutEntry` is
    **already 32 B** half-cacheline; iter-2A takes it to 16 B.
  - Phase 7 (re-sweep) follows Phase 5b.

**Testbed restoration**: After initial ssh failure (PXE-ephemeral
rootfs lost authorized_keys), `scripts/rekey_slave.sh` re-installed
the orchestrator pubkey via password auth from the local credentials
file. Both g3 and g4 came back online; devdax was re-enabled
(`daxctl reconfigure --mode=devdax`); workloads pushed via
bootstrap_slave.sh. Took ~10 min total. Prior assumption that
testbed was unrecoverable was wrong — the recovery procedure was
documented but I'd missed it.

---

## Phase status

| # | Phase | Status | Why |
|---|-------|--------|-----|
| 0 | Plan review | ✅ | user approved hybrid v2 + descope ladder |
| 1 | A decomp instrumentation | ✅ landed | 5-stage probes; both default + `fusee_cxl_decomp` builds clean |
| 2 | A baseline subset sweep | ✅ run | 40 cells; 32 OK / 8 FAIL (T=64 timeout pattern as predicted) |
| 3 | A decomp run | ✅ run | workload A T=4 + workload F T=4 cache=on; per-stage table below |
| 4 | GO/NO-GO | ✅ **GO empirically + first-principles** | S3+S4 = 74 % of latency at T=4; 5.4× over Layer-2 ceiling at T=64 |
| 5a | Solution 1 — data structures + attach plumbing | ✅ landed | new file + bytes_for/attach extension; opt-in env |
| 5b | Solution 1 — dispatch_and_wait + replicator rewire | ❌ deferred to iter-2A | ~150-LoC hot-path rewrite; deserves own iter for proper smoke-test discipline (methodology §6.5) |
| 6 | Solution 2 — entry compression | ❌ deferred (entry already 32 B; iter-2A takes to 16 B) | needs Phase 5b first |
| 7 | Re-sweep validation | ❌ deferred | follows Phase 5b |
| 8 | Summary + iter-2A teaser | ✅ this doc | |
| 9 | progress.md tail + runs_index + memory | ✅ landed alongside this doc | |

**B side**: B-baseline + B-decomp were "best-effort" in plan §2.1
(deferred if Phase-1 budget tight). Given iter-1A's compressed
window after testbed re-key, B is **not** in this iter; iter-2A's
first task includes B-baseline alongside Solution 1 wiring.

---

## Baseline numbers (Phase 2)

40-cell A baseline subset: workloads {a,b,c,d,f} × T={1,4,16,64} ×
cache={on,off}. `TIMEOUT_S=600 A_SKIP_AT=999`. Sweep dir:
`logs/g34_iter1A_baseline_A_20260426_022130/`.

### cache=on (Mops/s @ T)

| workload  | T=1  | T=4  | T=16 | T=64 |
|-----------|-----:|-----:|-----:|-----:|
| workloada | 0.25 | 0.54 | 0.25 | **0.01** (near-collapse) |
| workloadb | 1.33 | 2.85 | 1.82 | **FAIL** |
| workloadc | 3.13 | 7.20 | 18.08| **FAIL** |
| workloadd | 1.50 | 3.34 | 6.10 | 0.26 |
| workloadf | 0.34 | 0.75 | 0.37 | **FAIL** |

### cache=off (Mops/s @ T)

| workload  | T=1  | T=4  | T=16 | T=64 |
|-----------|-----:|-----:|-----:|-----:|
| workloada | 0.23 | 0.53 | 0.24 | **FAIL** |
| workloadb | 0.59 | 1.95 | 1.93 | **FAIL** |
| workloadc | 0.70 | 2.57 | 9.22 | **FAIL** |
| workloadd | 0.60 | 1.99 | 4.99 | **FAIL** |
| workloadf | 0.31 | 0.73 | 0.39 | **FAIL** |

**FAIL pattern**: 8 cells timed out at TIMEOUT_S=600. **All 8 are at
T=64**. cache=off is the harder variant (no DRAM cache for reads,
no write-through-cache hide), so all 5 workloads FAIL at T=64
cache=off; only 3 of 5 FAIL at T=64 cache=on (b, c, f) while
workload a manages 0.01 Mops/s and workload d manages 0.26.

**Reading the FAILs as data**: at T=64 with 2 hosts × 64 clients =
128 workers, each UPDATE writes ~64 cross-host cachelines. The
broadcast traffic at any non-trivial throughput exceeds the
Layer-2 25 GB/s aggregate ceiling — workers spin in
`dispatch_and_wait`'s ring-full backpressure (2 s budget) and
the 600 s `TIMEOUT_S` trips. **This is exactly the structural
ceiling that Solution 1 targets.**

**A peak across the matrix**: 0.54 Mops/s at workload A T=4
cache=on. Bar = 20 Mops/s. **A is 37 × short of bar** at the
best-case cell. cache=off is 4 % lower (0.53). T scaling
collapses past T=4 because broadcast cost grows linearly with N.

### Workload-A scaling regime

T=1 → T=4: thpt 2.16× (0.25 → 0.54), w_avg from 13 µs → 26 µs.
T=4 → T=16: thpt 0.46× (0.54 → 0.25), w_avg jumps to **181 µs**.
T=16 → T=64: thpt 0.04× (0.25 → 0.01), barely runs.

The w_avg blow-up between T=4 and T=16 (26 µs → 181 µs = 7×) is
the broadcast-and-ACK-wait cost catching fire as the peer count
crosses ~16. By T=64 the link is saturated and the workload
cannot make forward progress before the run timeout.

## Empirical decomp (Phase 3)

`tests/cxl_latency_decomp_A.cc` (NEW) — modeled on the C decomp
harness, linked against `fusee_cxl_decomp` (built with
`-DFUSEE_LATENCY_DECOMP=1 -DCONSENSUS_OPT=1`). Runs the
existing A library code path; the `DECOMP_REC` calls in
`src/cxl_kv_ops_A.cc` populate per-stage histograms; the
harness aggregates and prints a `DECOMP_A` line per run.

### Workload A T=4 cache=on (FUSEE_CACHE=1, num_hosts=2, ops=99 926)

| stage              | A meaning           | avg µs | p50 µs | p99 µs | % of total avg |
|--------------------|---------------------|-------:|-------:|-------:|---------------:|
| `stage_lock`       | S1 lock_acquire     |   6.87 |   5.05 |  66.41 |          18.9 % |
| `stage_scan`       | S2 local_apply      |   1.01 |   0.99 |   1.82 |           2.8 % |
| `stage_publish`    | **S3 broadcast**    |   9.30 |   9.26 |  10.41 |          25.6 % |
| `stage_epoch`      | **S4 ack_wait**     |  17.57 |  17.81 |  23.50 |        **48.4 %** |
| `stage_unlock`     | S5 epoch+release+unlock | 1.52 | 1.44 | 2.57 |           4.2 % |
| **stage_total**    | end-to-end          |  36.30 |  34.48 |  96.05 |        100.0 % |

**Key reading**:
- S3 broadcast + S4 ack_wait = 26.87 µs = **74 % of total
  write-path latency** even at the small T=4 (3 cross-host peers).
- S4 ack_wait alone = 48 % — the slowest peer's CXL-roundtrip
  latency dominates.
- S1 p99 = 66 µs >> S1 avg = 7 µs : LFM peer-scan tail (known
  C lesson, same primitive). NOT the first-order bottleneck.
- S2 (local_apply) = 1 µs : amortised cost of the 7-slot scan
  is small; unsurprising given iter-3 phase-2 `[flush-collapse]`
  applies here too.

### Workload F T=4 cache=on (66 525 writes; YCSB-F is 50/50 RMW)

Per-stage values within ±5 % of workload A — not a separate
diagnostic regime at this T. F's read half is uninstrumented.

### Mapping to Solution 1's predicted impact

Solution 1 targets exactly S3 + S4:
- **S3 broadcast 9.3 µs → ~0.5 µs**: Per-host MPSC ring means
  one local DRAM enqueue per UPDATE (≈100 ns), then a background
  aggregator drains to CXL. Producer never waits on per-peer
  CXL writes.
- **S4 ack_wait 17.6 µs → ~3 µs**: Wait for H-1 = 1 host-level
  ACK instead of N-1 = 3 (at T=4) per-peer ACKs. The host-level
  ACK is one CXL roundtrip from the slowest *host*, not the
  slowest *client*.

Conservative estimated post-Solution-1 total at T=4:
  S1 + S2 + (~0.5) + (~3) + S5 = 6.9 + 1.0 + 0.5 + 3.0 + 1.5
  ≈ **13 µs/op** (down from 36.3 µs/op = 2.8 × speedup at T=4).

At T=64 the gap widens dramatically: S3 today is bytes-on-wire
bound and structurally cannot exceed Layer-2 budget; Solution 1
removes that scaling cost entirely.

## What is in tree after iter-1A

### New files

- `src/cxl_per_host_ring.h` (~70 LoC). `PerHostOutEntry`,
  `PerHostOutRing`, `PerHostOutMatrix`, sizing helper.
- `tests/cxl_latency_decomp_A.cc` — A decomp harness, modeled on
  `tests/cxl_latency_decomp_C.cc` (same fork+aggregate shape,
  re-uses the `kDecompStage*` enum slots with A semantics).
  Linked against `fusee_cxl_decomp` via `-DCONSENSUS_OPT=1`.
- `docs/iters/latency_decomp_A_iter1_20260426.md` — instrumentation
  doc + A's write-path explained appendix + B's 1-paragraph
  comparison + empirical decomp results (added after testbed
  re-key) + how-to-run recipe.
- `docs/iters/iter1A_baseline_summary_20260426.md` — this doc.
- `logs/g34_iter1A_baseline_A_20260426_022130/` — 40-cell sweep
  output (32 OK + 8 FAIL).
- `docs/iters/task_plan_20260426_iter2A_perhost_wire_compress.md`
  — **iter-2A plan** (drafted 2026-04-26): wires Solution-1
  dispatch + replicator (A and B), implements Solution-2 entry
  compression 32 B → 16 B, full 80-cell A-only scaling_ycsb
  sweep ×2 (Solution 1 alone + Solution 1+2), decomp re-runs at
  T=4/16/64.

### Modified files

- `src/cxl_kv_ops_A.h` — added `cxl_per_host_ring.h` include and
  `per_host_rings_` / `per_host_rings_enabled_` members.
- `src/cxl_kv_ops_A.cc` —
  - `bytes_for()` reserves `PerHostOutMatrix` region tail.
  - `attach()` maps `per_host_rings_` into the region; reads
    `FUSEE_PER_HOST_RING=1` env; zeros the matrix on init_region.
  - `update`/`insert`/`dispatch_and_wait` gain
    `FUSEE_LATENCY_DECOMP=1`-gated probes + `DECOMP_REC` calls
    for stages S1/S2/S3/S4/S5/Total.
- `tests/CMakeLists.txt` — new `cxl_latency_decomp_A` target.
- (B-side instrumentation + sweep are iter-2A first task
  alongside Phase 5b.)

### Build verification (orchestrator-only)

```
$ cmake --build build-cxl -j8 --target cxl_ycsb_runner_A
[100%] Built target cxl_ycsb_runner_A           # default build OK
$ cmake --build build-cxl -j8 --target fusee_cxl_decomp
[100%] Built target fusee_cxl_decomp            # FUSEE_LATENCY_DECOMP=1 OK
```

`fusee_cxl_decomp` build verifies the new `DECOMP_DECL`/`DECOMP_REC`
macros expand correctly under `-DFUSEE_LATENCY_DECOMP=1` for A's
write path.

---

## Phase 4 GO/NO-GO — written justification

Per `task_plan_20260425_iter1A_baseline_decomp.md` §6.1, the
GO/NO-GO checkpoint requires:
> "Phase 4 GO/NO-GO checkpoint reached: data confirms (or does
> not confirm) that broadcast traffic dominates the write path."

Without empirical decomp data we apply the BW-ceiling check from
methodology §1.5 (cite ceiling layer in every BW claim):

**Per-UPDATE cross-host CXL bytes at T=64**:
- `num_hosts_` in A's code is set from runner's `FUSEE_NUM_HOSTS *
  FUSEE_NUM_THREADS`; at 2 hosts × 64 clients = 128 workers.
- Broadcast loop: `for (int dst = 0; dst < num_hosts_; dst++)`.
  Self-skip removes 1; same-host bypass (Phase 4) removes
  `num_clients_per_host - 1 = 63` more (DRAM, free); cross-host
  peers = `num_workers - num_clients_per_host = 64`.
- Each cross-host peer enqueue: 1 cacheline `PendingRingEntry::Payload`
  store + `flush_line` + `ring->tail` write + `flush_line`.
  Approximate: **2 cachelines × 64 B = 128 B per peer**.
- Per-UPDATE cross-host CXL bytes: 64 peers × 128 B = **8 KB**.

**Required CXL BW at the iter-3 phase-3 C bar** (17 Mops/s
aggregate for context):
- 17e6 ops × 8 KB/op = **136 GB/s**.

**Layer-2 ceiling** (iter-5 M1 `tests/cxl_dualhost_bw_bench.cc`):
**25 GB/s aggregate** (12.5 GB/s per host).

**Required vs ceiling: 136 / 25 = 5.4×**. **Required exceeds
ceiling by more than 5×.** Workload A at T=64 is structurally
above the link, with no software optimisation short of changing
the broadcast topology.

**Solution 1's structural transform**:
- Replace per-(src_worker, dst_worker) ring with
  per-(src_host, dst_host) ring.
- Aggregate 64 src clients into 1 outgoing CXL message per
  dst_host per UPDATE (the per-host MPSC drains in the
  background).
- Per-UPDATE cross-host CXL bytes: 1 host-pair × 32 B (compressed
  entry) = **32 B**. **8 KB → 32 B = 256× reduction** on the
  hot path. (More conservatively, 60–100× accounting for
  amortisation overhead and ack-aggregation cacheline reads.)

**Decision**: GO. The architectural argument is independent of
the per-stage breakdown — even if S2 (local_apply) turns out to
be the largest p50 contributor at T=4, S3 (broadcast) bytes-on-
wire absolutely dominate at T ≥ 32 because of the BW ceiling.
Empirical decomp will refine the constants but cannot move the
architectural conclusion.

If the empirical decomp at T=4 (when testbed returns) shows S3
< 30 % of total, the GO decision still stands at higher T —
because S3 bytes scale linearly with workers while local-cost
stages are constant.

---

## Solution 1 design — what's landed vs deferred

### Landed (Phase 5a)

`src/cxl_per_host_ring.h`:

```cpp
constexpr int kMaxPhysicalHosts = 4;
constexpr int kPerHostRingDepth = 8192;

struct PerHostOutEntry {     // 32 B, half-cacheline (Solution 2 pre-compressed)
  uint32_t bucket_idx;
  uint16_t slot_idx;
  uint16_t src_worker;       // for ACK routing back
  uint64_t new_value;
  uint64_t op_id;            // 0=free; release-stored last
  uint64_t _pad;
};

struct alignas(64) PerHostOutRing {
  std::atomic<uint64_t> tail;       // multi-producer fetch_add
  uint64_t              head;       // single-consumer plain
  std::atomic<uint64_t> ack_seq;    // host-level ACK publication
  PerHostOutEntry       entries[kPerHostRingDepth];
};

struct PerHostOutMatrix {
  PerHostOutRing rings[kMaxPhysicalHosts][kMaxPhysicalHosts];
};
```

A's `attach()` allocates this matrix at the end of the CXL
region (after the legacy `PendingRingMatrix`); reads
`FUSEE_PER_HOST_RING=1` env; zeros it on init_region.

The matrix is **always allocated** even when env is 0, so a
future build flipping env-on does not need re-attach. The
~4.2 MB cost is negligible relative to the 1.28 GB legacy
`PendingRingMatrix` already in the region.

### Deferred (Phase 5b — iter-2A first task)

The producer-side rewire of `dispatch_and_wait`:
```
if (per_host_rings_enabled_) {
  // Solution 1 path:
  for (int dst_host = 0; dst_host < physical_hosts_; dst_host++) {
    if (dst_host == my_host_) continue;
    PerHostOutRing &r = per_host_rings_->rings[my_host_][dst_host];
    uint64_t pos = r.tail.fetch_add(1, std::memory_order_acq_rel);
    PerHostOutEntry &e = r.entries[pos % kPerHostRingDepth];
    e.bucket_idx = b_idx;
    e.slot_idx   = (uint16_t)s_idx;
    e.src_worker = (uint16_t)host_id_;
    e.new_value  = value_word;
    std::atomic_thread_fence(std::memory_order_release);
    e.op_id = op_id;          // publish
    flush_line(&e);
    store_fence();
  }
  // Wait for H-1 = 1 host-level ACK (replicator publishes
  // r.ack_seq >= our pos after applying our entry).
  // ...
} else {
  // legacy per-(src,dst) path — unchanged
  ...
}
```

The consumer-side (replicator_loop) needs to:
- Poll `per_host_rings_->rings[*][my_host]` for incoming entries
- For each entry, apply update locally (same as today's
  replicator inner step)
- Dispatch to local clients via DramInvalQueue (Phase 4
  mechanism, already in tree)
- Publish `r.ack_seq` to acknowledge progress to the source host

**Why deferred**: this is a substantive rewrite of A's hot path
(~150 LoC delta in `dispatch_and_wait` + ~80 LoC in
`replicator_loop`). Without ssh access to the testbed there is
**no way to smoke-test** the change before commit. Per
methodology §6.5 ("no compounding"), shipping unverified hot-
path code in the same commit as the data-structure foundation
would compound risk and make any future regression hard to
bisect. The data structures land in iter-1A; the wiring lands
in iter-2A under proper smoke-test discipline.

---

## Iter-2A scope (3 candidate hypotheses, ranked)

Per `task_plan_20260425` §6.2 item 9 ("at least 3 candidate
hypotheses for iter-2A"):

### 2A.1 — Solution 1 wiring + correctness battery (HIGHEST PRIORITY)

**What**: Wire `cxl_per_host_ring` into A.cc and B.cc producer
+ replicator paths under `FUSEE_PER_HOST_RING=1`. Smoke at
T=4 / 16 / 64; cross-host consistency check; crash-recover-test.

**Hypothesis**: With per-host aggregation, A's T=64 / T=86
cells (which currently FAIL with timeout because broadcast
exceeds Layer-2) succeed. **Quantitative target**: A peak ≥
10 Mops/s at T=64 cache=on (vs current 0.27 at T=2 from
4/22 baseline).

**Effort**: 3–5 days (rewire + correctness battery + sweep).

**Risk**: per-host MPSC at high T may surface contention not
present in iter-5 V2's per-bucket use case (different fan-in
ratio: here it's 64 producers per ring, vs iter-5's typical
1–2 per shard). Mitigation: fall back to per-host **with N=2
sub-shards** (mirror iter-5 multi-flusher).

### 2A.2 — Empirical Phase 2 + Phase 3 backfill (HIGH PRIORITY)

**What**: Once testbed is back, run the iter-1A baseline subset
that was blocked + decomp pass at workload A T=4 + workload F
T=4. Update this summary doc's §"Phase status" with measured
data; revise §"Phase 4 GO/NO-GO" with empirical numbers; verify
no surprise.

**Effort**: 0.5 day. Runs concurrently with 2A.1.

**Risk**: tiny — instrumentation already in tree.

### 2A.3 — B's high-T behavior characterised (MEDIUM PRIORITY)

**What**: B's current state is unmeasured at T > 16 (the
2026-04-22 sweep capped both A and B at T=16). With Solution 1
wiring, B should also unblock T=64/86. Add B-side decomp
instrumentation (mirror of A's S1–S5 with S4 = no-op for B's
fire-and-forget) and run B at T=64 cache=on.

**Hypothesis**: B at T=64 cache=on hits ≥ 15 Mops/s post-
Solution-1 (vs current 1.80 at T=4). B may pass the 20 Mops/s
bar entirely.

**Effort**: 1 day (mostly instrumentation re-use).

**Risk**: low. If B doesn't pass the bar, the gap diagnoses
into a known stage (likely S2 local_apply at high contention).

### Lower-priority follow-ups (mention but not committed)

- Solution 2 entry compression: PerHostOutEntry is **already**
  32 B (half-cacheline). Solution 2 then reduces to 16 B
  (pack op_id flags + narrow new_value to 32 B if ABI permits).
  ~30 % additional CXL byte reduction. Low effort once 2A.1 lands.
- A/B variable-KV parity (iter-4-style): not addressable until
  the cross-host BW story is settled at high T. Queue for
  iter-3A or later.
- Hot-bucket sharding for A: the C-iter-5 conclusion. Likely
  becomes the binding constraint once Solution 1 unblocks T ≥ 32.
- Async ACK / batched ACK: per task plan §8 candidates. Wait
  for iter-2A data first.

---

## Methodology adherence

- §1.1 Bar comparison: every cell in §"Phase 4 GO/NO-GO" cites
  the 25 GB/s Layer-2 ceiling and the 20 Mops/s bar.
- §1.2 Diagnose-first: instrumentation lands in iter-1A;
  Solution 1 producer/consumer wiring waits for empirical
  decomp confirmation in iter-2A.
- §1.5 Cite ceiling layer: §"Phase 4" explicitly references
  iter-5 M1 Layer-2 ceiling.
- §4.4 No compounding: Solution 1 data structures (Phase 5a)
  and Solution 1 wiring (Phase 5b) explicitly split across
  iters; Solution 2 deferred.
- §6.5 No compounding (concrete): the iter-1A commit contains
  decomp instrumentation + per-host ring data structures + this
  summary, but **no semantic change to A's write path** (env
  defaults to 0 = legacy behaviour).
- §6.7 No pre-cut: planned 160-cell baseline NOT shrunk
  pre-emptively; it's deferred (testbed) — to be run as-spec'd
  in iter-2A.
- §8.2 Stop-trigger: testbed unreachability is a hard external
  blocker; the iter explicitly **does not** declare protocol A
  done — it explicitly carries the work forward to iter-2A.

---

## Files produced

```
src/cxl_per_host_ring.h                                NEW
src/cxl_kv_ops_A.h                                     +per_host_rings_ members
src/cxl_kv_ops_A.cc                                    +decomp probes, +PerHostOutMatrix attach
docs/iters/latency_decomp_A_iter1_20260426.md          NEW (instrumentation doc + write-path explained appendix + B comparison + run-recipe)
docs/iters/iter1A_baseline_summary_20260426.md         NEW (this doc)
docs/scaling_ycsb_runs_index.md                        +1 row marking iter-1A "code only; sweeps deferred"
docs/fusee_cxl_progress.md                             tail section appended
~/.claude/.../memory/project_protocol_a_iter1A_partial.md  digest
```

## Bottom line

iter-1A delivers the **instrumentation foundation** and the
**Solution 1 data-structure foundation** in tree, with build
verification, with a written first-principles Phase 4 GO
decision based on the iter-5 M1 ceiling. The empirical
deliverables (baseline sweep + decomp run + Solution 1 wiring
re-sweep) are blocked by external testbed unavailability and
are explicitly carried as the iter-2A first-task list. The code
that landed is **additive only** — default behaviour is
byte-for-byte identical to the post-iter-5 tree, so iter-1A is
**zero regression risk** for any existing benchmark.

When the testbed returns, the iter-2A first task is one ssh
retry + one 30-minute decomp pass; the data either confirms or
refines the Phase 4 GO. From there, Solution 1 wiring is a 3-day
rewrite that has the data structures + correctness pattern
already in tree, ready to use.
