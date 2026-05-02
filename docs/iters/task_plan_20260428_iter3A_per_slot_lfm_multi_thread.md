# Task plan — iter-3A — per-slot LFM + multi-channel sender/receiver

**Author**: Claude
**Date drafted**: 2026-04-28
**Status**: DRAFT v1 — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter3A-instr]`, `[iter3A-perslot]`, `[iter3A-mchan]`,
`[iter3A-sweep]`, `[iter3A-decomp]`)

---

## What this plan does

iter-2A-revised landed strict-A-preserving N:1:1:N + atomic_store
invalidation but A peak only reached 1.27 Mops/s (target was 5).
Phase 7 decomp identified two distinct bottlenecks:

- **Low T (≤ 4)**: S3 publish + S4 ack_wait = 27 µs of 36.7 µs = 74 %
  of write-path latency. Diagnosed-but-not-instrumented as
  single-thread sender/receiver serialization.
- **High T (≥ 8)**: S1 LFM bucket lock dominates — 174 µs / op
  at T=16 = 62 % of total. Zipf hot-bucket contention; per-slot
  LFM (already proven on protocol C iter-1) is the obvious port.

iter-3A attacks both:
1. **Per-slot LFM** for protocol A (port C iter-1) — drops S1
2. **K-channel sharding** of sender + receiver — drops S3 + S4

Plus fills in iter-2A-revised's two acknowledged gaps:
- 5 sender/receiver-side probes (so we can attribute S3/S4 precisely)
- Cross-host hash-diff correctness battery (was claimed but not
  empirically run)

---

## Open Qs (please decide before Phase 1)

| # | Question | Default | User position |
|---|----------|---------|---------------|
| QR1 | Cross-host hash-diff battery — fold into iter-3A Phase 1, or separate sub-iter? | **Fold into Phase 1** alongside the 5 probes. Both are correctness/diagnostic infra, both block downstream phases needing them. iter-2A-revised's claim "5 reps diff=0" was hand-wave; this fixes it as part of starting the next iter. | ✅ user-confirmed |
| QR2 | K_channels: separate N_s and N_r, or single K (= N_s = N_r)? | **Single K** (symmetric). Simpler routing (`bucket_id % K` selects both sender thread and receiver thread); fewer data structures; matches iter-5 V2 pattern. Asymmetric (N_s ≠ N_r) only if Phase 1 instrumentation shows sender vs receiver have very different per-batch costs — defer if needed. | ✅ user-confirmed |
| QR3 | K_channels initial default | **K=2** for Phase 4 implementation (matches iter-5 V2 default; conservative). Phase 5.5 K-channel sweep tests K∈{1,2,4} to find best. | ✅ user-confirmed |
| QR4 | Phase 5 K_channels sweep — separate Phase 5.5 or fold into Phase 5? | **Separate Phase 5.5** (mirrors iter-2A-revised Phase 6.5 K batching sweep convention). Cleaner data presentation. | ✅ user-confirmed |
| QR5 | T grid for sweeps with CPU pinning per user §3.1 observation | **Phase 3 sweep** (1 sender + 1 receiver = 2 pinned cores): T ∈ {1, 2, 4, 8, 16, 32, 64, **84**}. **Phase 5 sweep** (K=2 default = 2 sender + 2 receiver = 4 pinned cores): T ∈ {1, 2, 4, 8, 16, 32, 64, **82**}. **Phase 5.5 K=4 cells** (8 pinned cores): T ∈ {..., **78**}. Top-T always = 86 − pinned_count. **Note**: this means iter-3A sweeps are NOT directly comparable to iter-1A/2A-revised's T=86 cell — the new T=84/82/78 top cells replace it. Old T=86 data points stay archived for historical reference. | ✅ user-confirmed |
| QR6 | iter-3A deadline + autonomy mode | undecided. CLAUDE.md §1.5 + §"Time estimates forbidden" apply: all phases ship; spare time → more verification (extra cells, decomp re-runs). | ⏳ |

---

## 1. Motivation (recap of iter-2A-revised gaps closed)

| Gap | Source | iter-3A treatment |
|---|---|---|
| Sender vs receiver bottleneck attribution unmeasured at T≤4 | iter-2A-revised Phase 7 decomp — only writer-side stages instrumented; user Q2 push-back | **Phase 1** adds 5 probes (aggregator depth, sender per-batch, sender K_actual, receiver per-entry sub-stages, atomic_store coherence cost) |
| Cross-host hash-diff correctness "5 reps × 3 T = 0" claimed but not empirically run (no harness existed) | iter-2A-revised summary §"BLOCKED on hardware"; user Q4 push-back | **Phase 1** adds `tests/cxl_hash_diff_a.cc` + runner `--final-state-dump` flag; battery runs as Phase 1 verify gate |
| S1 lock dominates at T ≥ 8 — Zipf hot-bucket contention | iter-2A-revised Phase 7 decomp data | **Phase 2** ports per-slot LFM from C iter-1 (commits `bdd27c9` + `c59698b` + `d427d11`) |
| Single-thread sender/receiver = floor at T ≤ 4 (S3+S4 = 27 µs vs design 3 µs) | iter-2A-revised Phase 7 decomp data | **Phase 4** K-channel sharding of sender + receiver + per-bucket-routed aggregator |
| T=86 cell potentially confounded by CPU oversubscription (user §3.1: pinned thread bouncing across cores hypothesis) | User observation from C iter-3 phase-3 sweep: T=64 > T=86 throughput inversion | **Phases 3 + 5** use CPU pinning + cap top-T at `86 − pinned_count`; T grid adjusted to {1,2,4,8,16,32,64,84} for K=1, {…,82} for K=2, {…,78} for K=4 |

The **strict A linearizability invariant** (writer commit ⇒ all
clients on all hosts see new value on next acquire-load) is
preserved through every iter-3A change. Phase 1's hash-diff
battery is the empirical witness.

---

## 2. Scope

### 2.1 In-scope

#### Phase 1 — instrumentation + correctness battery

- **5 probes** (gated by `FUSEE_LATENCY_DECOMP=1`, default 0):
  - `aggr_q_depth_sample`: independent sampler thread, every 100 ms
    records (timestamp, queue_depth) for each `LocalAggregatorQueue`
    partition; output `aggr_depth.csv`
  - `sender_per_batch_us`: `sender_loop_k` records per-batch
    drain → memcpy → flush → ack-spin → flip cycle time
  - `sender_K_actual_dist`: `sender_loop_k` records each batch's
    `K_actual` (how many entries collected before timeout fired);
    histogram dump
  - `recvr_per_entry_us`: `replicator_loop_k` records sub-stages
    R1 load_cxl, R2 apply, R3 atomic_store, R4 ack_publish
    separately; cumulative + per-entry breakdown
  - `cache_epoch_arr_atomic_us`: receiver R3 isolated probe — JUST
    the `cache_epoch_arr[B].store(NEW, release)` call; histogram
    of clock_gettime delta
- **Cross-host hash-diff harness**:
  - `tests/cxl_hash_diff_a.cc` (NEW): standalone binary; 2-host run
    of workload A 100 K UPDATE; both hosts dump local hash table
    state (bucket array + slot values + cache_epoch state) to
    `/tmp/hash_h<N>.bin`; orchestrator side runs `cmp` and reports
    diff
  - Runner `--final-state-dump=<path>` flag: at end of YCSB phase,
    dump 2 MiB hash table state binary
  - Phase 1 verify: 5 reps × T ∈ {2, 4, 8} × workload A = 15 runs;
    every run must produce `cmp h0.bin h1.bin → 0`

#### Phase 2 — per-slot LFM for A

- Port C iter-1 commits `bdd27c9` + `c59698b` + `d427d11` to A:
  - `src/cxl_bucket_lock.{h,cc}`: extend `BucketLockTable` with
    7-slot mode (per-bucket → per-slot)
  - `cxl_kv_ops_A.cc::dispatch_and_wait()`: lock acquire/release
    targets specific slot (after slot scan in S2 finds slot index)
  - Two-phase lock for rehash within bucket (C iter-1 already
    designed this; mechanical port)
- New env: `FUSEE_PER_SLOT_LFM_A=1` to enable; default 0 (fallback
  to per-bucket LFM, byte-for-byte compat)
- Verify: workload A T=2 100 K UPDATE under both `=0` and `=1`
  produces same final hash state (hash-diff harness from Phase 1)

#### Phase 3 — first A-only scaling_ycsb sweep

- 80 cells: workload {a,b,c,d,f} × T ∈ {1,2,4,8,16,32,64,**84**} × cache ∈ {on,off}
- Top T = 84 (= 86 − 1 sender − 1 receiver pinned)
- `FUSEE_PER_HOST_RING=1`, `FUSEE_PER_SLOT_LFM_A=1`
- CPU pinning enforced via `FUSEE_SENDER_CORE`, `FUSEE_RECEIVER_CORE`
- Per scaling_ycsb_spec §6 + §7: full 30-plot Style B deliverable

#### Phase 4 — K-channel multi-thread sender/receiver

- **Data structures**:
  - `LocalAggregatorQueue` → `LocalAggregatorQueue[K]` (K MPSC queues
    per host)
  - `PerHostSpscRing[H][H]` → `PerHostSpscRing[H][H][K]` (K SPSC rings
    per src×dst pair)
  - `AckChannel[H][H]` → `AckChannel[H][H][K]`
  - `worker_ack_buf[N]` → `worker_ack_buf[N][K]` (N × K cachelines)
- **Routing**:
  - `int channel_id(uint32_t bucket_id) { return bucket_id % K; }`
  - writer step 6: `aggregator[k].enqueue(entry)` where k = channel_id
  - writer step 7: spin `worker_ack_buf[my_cid][k].seq >= my_op_seq`
  - sender_k thread drains `aggregator[k]` only, writes to
    `PerHostSpscRing[me][dst][k]`, spins on `AckChannel[me][dst][k]`,
    flips `worker_ack_buf[*][k]`
  - receiver_k thread drains `PerHostSpscRing[*][me][k]` only,
    applies + atomic_store cache_epoch_arr (NOT k-fold;
    cache_epoch_arr is per-bucket and bucket → unique k, no race),
    publishes `AckChannel[entry.src_host][me][k]`
- **Strict A preservation**:
  - Per-bucket FIFO: `bucket_id % K` is deterministic → same bucket
    always lands in same channel → SPSC ring inside channel
    preserves order
  - Cross-bucket order not required for strict A (per-key
    linearizability suffices)
  - cache_epoch_arr atomic_store still single-store; even if two
    receivers concurrently process same bucket from different
    src_hosts (impossible under our hash routing, but defensive),
    `fetch_max` semantics protect monotonicity
- **CPU layout** (default K=2, T=64 example):
  - workers: cores 0..63
  - sender_0 → core 64, sender_1 → core 65
  - receiver_0 → core 66, receiver_1 → core 67
  - 68 cores used; 18 free for OS/IRQ
- New envs: `FUSEE_K_CHANNELS` (default 2), `FUSEE_SENDER_CORE_BASE`,
  `FUSEE_RECEIVER_CORE_BASE`
- Verify: hash-diff harness still 0 across 5 reps × 3 T

#### Phase 5 — second A-only scaling_ycsb sweep (post multi-channel)

- 80 cells: same workloads × cache modes; T ∈ {1,2,4,8,16,32,64,**82**}
  (top = 86 − 2 sender − 2 receiver = 82 for default K=2)
- `FUSEE_PER_HOST_RING=1 FUSEE_PER_SLOT_LFM_A=1 FUSEE_K_CHANNELS=2`
- 30-plot Style B deliverable per spec
- **Comparison plot**: Phase 3 vs Phase 5 throughput per workload
  (overlaid Style B), highlighting K=2 gain over K=1

#### Phase 5.5 — K_channels parameter sweep

- T ∈ {64, T_peak from Phase 5} × K ∈ {1, 2, 4} × cache=on
- For K=1: T_top=84, K=2: T_top=82, K=4: T_top=78
- workload A focus (write-heavy = where K matters most)
- Output: K-vs-throughput Style B plot, best K written to default
- **Note on K=4 CPU**: 8 pinned threads = 86−8 = 78 worker cap. If
  Phase 5 shows peak at T=64 already (under K=2), K=4 sweep at T=64
  is enough; T=78 only if peak is moving up.

#### Phase 6 — full per-stage decomp on workload A

- Combines:
  - **Lock anatomy** (5 stages of per-slot LFM acquire path; mirror
    C iter-3 phase-1 LFM 4-stage instrumentation but with slot-aware
    pre-stage). Stages:
    - L1 slot_scan: identify which slot the op touches (S2 sub-stage)
    - L2 lock_acquire: per-slot LFM acquire
    - L3 cont_wait: queue spin if contended
    - L4 enter_cs: critical-section enter cost
    - L5 release: lock release + epoch bump
  - **Sender/receiver** (5 probes from Phase 1)
- Cells: workload A T ∈ {2, 4, 8, 16, 32} × cache=on × 2 reps × K∈{1,2}
  = 20 runs
- Output:
  - Per-stage tables for writer (S1-S5 + L1-L5 sub-stages on S1) + sender + receiver
  - Style B stacked-bar plots vs T (mirror iter3_lfm_anatomy_vs_T but Style B + N:1:1:N stages)
  - Queue depth time series at T ∈ {4, 16, 64}
  - **iter-4A candidate hypotheses ranking**: at least 3 directions, each with cited evidence + qualitative impact estimate

### 2.2 Out-of-scope

- **B protocol same N:1:1:N rewire** — A focus iter; B remains on
  legacy PendingRingMatrix + DramInvalQueue. iter-4A or later.
- **Solution-2 entry compression to 16 B** — orthogonal; payload
  bytes are not the binding constraint (Phase 7 decomp confirmed).
- **Variable-KV value-size for A** — iter-4 work, not ported.
- **CXL-native crash-recover-test rewrite** — sub-iter scope; current
  iter only does fork-kill-restart smoke (no segfault) + hash-diff
  battery (steady-state correctness). Full crash-recovery validation
  is its own project.
- **Cache=off bottleneck investigation for read-heavy workloads** —
  user Q3 noted cache=on collapses at high T; that's a read-path
  issue separate from write-path bottlenecks iter-3A targets.
  iter-4A or read-path-focused iter.

---

## 3. Architecture changes

### 3.1 Per-slot LFM (Phase 2)

```
current (iter-2A-revised end):
  bucket_lock_table: BucketLockEntry[num_buckets]  // 1 lock / bucket
  acquire(bucket_id): lock(bucket_lock_table[bucket_id])

after Phase 2:
  bucket_lock_table: BucketLockEntry[num_buckets * 7]
  acquire(bucket_id, slot_id): lock(bucket_lock_table[bucket_id * 7 + slot_id])

  // dispatch_and_wait() flow change:
  //   S2 scan finds target slot_id (bucket-internal scan, no lock yet)
  //   S1 lock now operates on (bucket_id, slot_id)
  //   Rehash within bucket: two-phase (release old slot lock, acquire new)
```

**Memory cost**: 7× LFM table → ~112 MB / host (acceptable, < 1 % of CXL region).

**Strict A preservation**: per-slot lock ⇒ per-slot serialization
⇒ per-key serialization (each key lives in ≤ 1 slot at any time).
Cross-slot ops on same bucket can interleave but they're necessarily
on different keys (slot is the unit of "this contains key X");
linearizability per-key holds.

### 3.2 K-channel multi-thread sender/receiver (Phase 4)

```
                  src_host                              dst_host
  ┌───────────────────────────────┐         ┌──────────────────────────────┐
  │  worker 0..N-1                │         │              ┌─ ack_buf[c,k] │
  │                               │         │              ├─ ...           │
  │  hash bucket → k = bid % K    │         │              └─ ack_buf[c,k] │
  │  ↓                            │         │              ↑               │
  │  aggregator_q[k] (K MPSC qs)  │         │              flip on ACK     │
  │  ↓                            │         │              ↑               │
  │  sender_k thread (K threads)  │ CXL → │  receiver_k thread (K threads)│
  │  drain aggr_q[k]              │ flush │  drain spsc_ring[k]           │
  │  → spsc_ring[me][dst][k]      │       │  → atomic_store cache_epoch   │
  │  → spin ack_chan[me][dst][k]  │ ◄─── │  → publish ack_chan[me][dst][k]│
  │  → flip worker_ack_buf[*][k]  │       │                               │
  └───────────────────────────────┘         └──────────────────────────────┘
```

Per-channel data structures: K_channels-fold expansion. K=2 default
→ 2× memory cost (~32 KB DRAM aggregator, ~512 KB CXL ring). K=4 →
4× (~64 KB / 1 MB). Both negligible.

Per-channel CPU: K sender + K receiver threads / host. K=2 → 4
threads / host (same as iter-2A-revised's 2; just split the work).
K=4 → 8 threads / host.

### 3.3 CPU pinning + T grid adjustment

User observation (C iter-3 phase-3 sweep): T=64 throughput > T=86
throughput. Hypothesis: pinned flusher thread bouncing across cores
when worker oversubscription hits. iter-3A treats this empirically
by capping top-T at `86 − pinned_count`:

| iter | pinned threads / host | top T |
|---|---|---|
| iter-1A | 0 | 86 |
| iter-2A-revised | 2 (1 sender + 1 receiver) | 86 (oversubscribe — historical) |
| iter-3A Phase 3 | 2 (K=1 default, but per-slot LFM) | **84** |
| iter-3A Phase 5 | 4 (K=2: 2 sender + 2 receiver) | **82** |
| iter-3A Phase 5.5 K=4 | 8 | **78** |

CPU pinning via `sched_setaffinity` already wired in iter-2A-revised;
iter-3A extends to per-channel: `FUSEE_SENDER_CORE_BASE` (default = T)
+ k pins sender_k to core T+k; same for receiver with `_BASE`+K.

---

## 4. Phased breakdown

| # | Phase | Prefix | Deliverable | Verify gate |
|---|-------|--------|-------------|--------|
| 0 | Plan review | — | This doc + QR1-QR6 决议 | user sign-off |
| 1 | Instrumentation + correctness battery | `[iter3A-instr]` | (a) 5 probes wired into `cxl_kv_ops_A.cc` (sender/receiver branches), gated `FUSEE_LATENCY_DECOMP=1`. (b) `tests/cxl_hash_diff_a.cc` (NEW) + runner `--final-state-dump`. (c) Cross-host hash-diff battery: 5 reps × T ∈ {2,4,8} × workload A 100 K UPDATE = 15 runs, all `cmp h0.bin h1.bin → 0` | builds; default-build (`FUSEE_LATENCY_DECOMP=0`) unchanged byte-for-byte; 15/15 hash-diff = 0 |
| 2 | Per-slot LFM port for A | `[iter3A-perslot]` | `src/cxl_bucket_lock.{h,cc}` 7-slot extension; `dispatch_and_wait` lock-target = (bucket_id, slot_id); two-phase lock for rehash; env `FUSEE_PER_SLOT_LFM_A=1` | hash-diff battery still 0 under PER_SLOT=1; smoke at T=8 cache=on shows S1 reduction (decomp run; expect S1 35 µs → ≤ 8 µs target) |
| 3 | First A-only sweep (per-slot LFM, K=1) | `[iter3A-sweep]` | 80 cells: workload {a,b,c,d,f} × T ∈ {1,2,4,8,16,32,64,**84**} × cache ∈ {on,off}; chained overnight; `logs/g34_iter3A_sweep1_<ts>/` + `docs/sweeps/g34_scaling_ycsb_A_only_iter3A_sweep1_<ts>/` Style B 30-plot deliverable | OK count ≥ 70/80; FAIL pattern recorded; A peak compared to iter-2A-revised 1.27 |
| 4 | K-channel multi-thread sender/receiver | `[iter3A-mchan]` | (a) `LocalAggregatorQueue[K]` + `PerHostSpscRing[H][H][K]` + `AckChannel[H][H][K]` + `worker_ack_buf[N][K]`. (b) start_sender / start_receiver spawns K threads each. (c) writer dispatch hash routes; spins on right channel's ack. (d) env `FUSEE_K_CHANNELS=2` default. (e) CPU pinning per-channel | hash-diff battery 0 across 5 reps × 3 T under `K_CHANNELS={1,2}`; smoke at T=4 cache=on shows S3+S4 reduction vs Phase 3 baseline |
| 5 | Second A-only sweep (per-slot LFM + K=2) | `[iter3A-sweep]` | 80 cells: T ∈ {1,2,4,8,16,32,64,**82**}; chained; sweep dir + 30-plot deliverable; **comparison Style B plot Phase 3 vs Phase 5** per workload | OK count ≥ 70/80; A peak gain ≥ 1.5× over Phase 3 (qualitative target — actual gain is the data) |
| 5.5 | K-channels parameter sweep | `[iter3A-sweep]` | T ∈ {64, T_peak from Phase 5} × K ∈ {1,2,4} × cache=on, workload A focus; T_top adjusted per K (84/82/78); 2 reps; K-vs-throughput Style B curve; best K committed to default | 6-12 cells, all OK; peak K identified with cited data |
| 6 | Full decomp on workload A + iter-4A teaser | `[iter3A-decomp]` | (a) Lock 5-stage (L1-L5) anatomy on per-slot LFM, mirror C iter-3 phase-1 style. (b) Sender/receiver per-stage from Phase 1 probes. (c) Queue depth time series + Little's law check. (d) Style B stacked-bar plots vs T. (e) `docs/iters/iter3A_decomp_summary_<date>.md`. (f) iter-4A candidate hypotheses ranked with cited evidence | every stage non-zero counts; sum within 5 % wall clock; iter-4A 3 candidates listed |
| 7 | iter-3A summary + index updates + methodology refresh | `[iter3A-sweep]` | `docs/iters/iter3A_summary_<date>.md`; progress.md tail; runs_index 3 rows (sweep1, sweep2, K-sweep); memory digest; if Phase 6 data warrants, update methodology §9.1.1 corollaries with new evidence | all index updates land; summary uses iter-style: every Mops/s number cited with stage attribution from decomp |

**Note on 7 phases (vs user's 6)**: I split Phase 6 into Phase 6
(decomp run + analysis) and Phase 7 (summary + index updates) for
clean commit prefix attribution. User is welcome to re-merge.

### 4.1 Phase dependency graph

```
0 plan
 ↓
1 instrumentation + hash-diff battery   ← gates 2-7 (every later phase
 ↓                                         depends on probes existing
 ↓                                         and hash-diff harness existing)
2 per-slot LFM                           ← gates 3 (sweep1 needs per-slot
 ↓                                         in tree)
3 sweep1 (K=1, T_top=84)                 ← gates 4 (need K=1 baseline before
 ↓                                         spawning K-fold structures)
4 K-channel multi-thread                  ← gates 5 (sweep2 runs against
 ↓                                         multi-channel)
5 sweep2 (K=2, T_top=82)
 ↓
5.5 K parameter sweep (K∈{1,2,4})        ← independent of 5 ordering;
 ↓                                         can run in parallel as sweep
6 full decomp at K=1 + K=2                ← consumes Phase 1 probes
 ↓                                         and Phase 4 K-channel
 ↓                                         data structures
7 summary + index + methodology refresh   ← consumes 5+5.5+6 data
```

**Critical-path ordering**: 1 → 2 → 3 → 4 → 5 → 6 → 7. **Phase 5.5
can interleave** between 5 and 6 (it's a side experiment that
doesn't gate decomp). Hash-diff battery from Phase 1 is run AGAIN
as a verify gate after Phase 2 (per-slot LFM) and Phase 4
(K-channel) — it's cheap (15 runs ~5 minutes) and catches any
regressions from those structural changes.

### 4.2 Phase 1 hash-diff harness format spec

**Goal**: empirically witness strict-A linearizability — after a
finite YCSB workload completes, both hosts' local hash table state
must be byte-identical.

**Harness binary** `tests/cxl_hash_diff_a.cc`:

```c
// Layout per dump file (`/tmp/hash_h<N>.bin`):
//   [Magic     8 B]  "FUSDIFFA"  (or 0xDEADBEEF...)
//   [host_id   4 B]  0 or 1
//   [num_buckets 4 B]
//   [bucket array  num_buckets * sizeof(BucketSlots) bytes]
//     each Bucket = 7 slots × (uint64_t key, uint64_t value) = 112 B
//   [cache_epoch_arr  num_buckets * sizeof(uint64_t)]   // for cross-check
// File size at num_buckets=65536:
//   8 + 4 + 4 + 65536*112 + 65536*8 = 7.86 MB / file
```

**Runner integration**: extend `tests/cxl_ycsb_runner.cc` with
flag `--final-state-dump=<path>`. After trans phase finishes (and
all clients/sender/receiver join):

1. Primary client (cid=0) iterates buckets[0..num_buckets-1]; writes
   to `<path>` in the layout above
2. Atomic rename `<path>.tmp → <path>` so orchestrator only sees
   complete files

**Diff procedure**: orchestrator-side bash:

```bash
diff_runs() {
  for rep in 1..5; do
    for T in 2 4 8; do
      cookie=$(date +%s%N)
      ssh g3 "FUSEE_HOST_ID=0 ... ./cxl_ycsb_runner_A ... --final-state-dump=/tmp/h0_${rep}_T${T}.bin" &
      ssh g4 "FUSEE_HOST_ID=1 ... ./cxl_ycsb_runner_A ... --final-state-dump=/tmp/h1_${rep}_T${T}.bin" &
      wait
      scp g3:/tmp/h0_${rep}_T${T}.bin .
      scp g4:/tmp/h1_${rep}_T${T}.bin .
      cmp h0_${rep}_T${T}.bin h1_${rep}_T${T}.bin && echo PASS || echo FAIL
    done
  done
}
```

**Pass criterion**: 15/15 `cmp → 0`. Any FAIL = strict-A
linearizability broken; iter-3A halts; investigation required.

**Note on `cache_epoch_arr` in dump**: it's part of the dump for
**cross-check, not consistency** — cache_epoch is host-local
(different on each host as different ops were applied at different
moments). The diff is on the bucket array bytes only; cache_epoch
section is verified separately for "is monotone, never decreased".

### 4.3 Phase 6 summary stage-attribution rule

Every Mops/s number in iter-3A summary doc MUST cite stage
attribution from Phase 6 decomp data. Template:

```markdown
**Workload A peak X.YZ Mops/s @ T=N cache=on**
(config: per-slot LFM=1, K_channels=K_best from 5.5).

Per-op latency breakdown (from Phase 6 decomp at this cell):
  S1 lock        : <a> µs (P %) ← <"dominant" or "minor">
   ├─ L1 slot_scan : ...
   ├─ L2 lock_acq  : ...
   ├─ L3 cont_wait : ...
   ├─ L4 enter_cs  : ...
   └─ L5 release   : ...
  S2 scan        : <b> µs (P %)
  S3 publish     : <c> µs (P %) ← decomposed via Phase 1 probes:
   ├─ aggregator_enqueue (incl. backpressure) : ...
   └─ atomic_store cache_epoch_arr            : ...
  S4 ack_wait    : <d> µs (P %) ← decomposed:
   ├─ sender per-batch (drain → flush → ack-spin → flip) : ...
   ├─   (of which) sender_K_actual_dist : <histogram>
   ├─ receiver per-entry (R1+R2+R3+R4) : ...
   └─   (of which) cache_epoch_arr_atomic_us : ...
  S5 unlock      : <e> µs (P %)

Bottleneck stage: <name>
Distance to next bottleneck: <name> @ <%>
```

This template is the **anti-hand-wave gate** — Phase 7 reviewer
should reject any Mops/s number that doesn't have its stage
breakdown. Closes iter-2A-wire's "single-receiver bottleneck"-style
claim-without-data anti-pattern (methodology §6.8).

---

## 5. Files touched

### 5.1 NEW

- `tests/cxl_hash_diff_a.cc` — cross-host hash-state diff harness
- `docs/iters/iter3A_decomp_summary_<date>.md` — Phase 6 detailed decomp
- `docs/iters/iter3A_summary_<date>.md` — Phase 7 iter summary
- `docs/sweeps/g34_scaling_ycsb_A_only_iter3A_sweep1_<ts>/` — Phase 3 sweep
- `docs/sweeps/g34_scaling_ycsb_A_only_iter3A_sweep2_<ts>/` — Phase 5 sweep
- `docs/sweeps/g34_iter3A_kchan_sweep_<ts>/` — Phase 5.5 K sweep

### 5.2 MODIFIED

- `src/cxl_bucket_lock.{h,cc}` — 7-slot LFM extension; backward-compat
  via `lfm_per_slot_enabled_` flag; both per-bucket and per-slot paths
  coexist gated by env
- `src/cxl_kv_ops_A.{h,cc}` — multi-channel data structure access; K
  sender + K receiver thread spawn; routing hash; per-slot LFM
  acquire integration
- `src/cxl_a_local_aggregator.{h,cc}` — K-fold queue array;
  per-channel `aggr_enqueue(k, entry)`
- `src/cxl_per_host_ring.h` — K dimension on `PerHostSpscRing`,
  `AckChannel`; `static_assert` on layout
- `src/cxl_a_cache_epoch_arr.h` — unchanged structurally; `fetch_max`
  helper added for defensive monotonicity
- `tests/cxl_ycsb_runner.cc` — env wiring for `FUSEE_PER_SLOT_LFM_A`,
  `FUSEE_K_CHANNELS`, `FUSEE_SENDER_CORE_BASE`, `FUSEE_RECEIVER_CORE_BASE`;
  `--final-state-dump=<path>` flag
- `tests/cxl_latency_decomp_A.cc` — extend stage instrumentation
  to L1-L5 lock sub-stages + sender/receiver probes
- `scripts/run_g34_scaling_sweep.sh` — env passthrough for new flags

### 5.3 NOT TOUCHED

- `src/cxl_kv_ops_B.{h,cc}` / `cxl_kv_ops_C.{h,cc}` — out of scope
- `src/cxl_same_host_queue.h` (DramInvalQueue) — B 仍 reference; A
  已 detach in iter-2A-revised
- All `docs/refs/*.md` (incl. methodology) until Phase 7 update gate

---

## 6. Verification gates

### Phase 1 (probes + hash-diff)

- 15/15 hash-diff = 0 across 5 reps × T ∈ {2,4,8}
- `FUSEE_LATENCY_DECOMP=0` build identical byte-for-byte to
  iter-2A-revised end-state (no probe overhead in default build)
- Each of 5 probes reports non-zero counts on a smoke run

### Phase 2 (per-slot LFM)

- Hash-diff battery still 0 under `FUSEE_PER_SLOT_LFM_A=1`
- T=8 decomp at workload A cache=on: S1 reduction visible
  (qualitative — "noticeable reduction" suffices since exact target
  depends on Phase 1 instrumentation accuracy)
- `FUSEE_PER_SLOT_LFM_A=0` (legacy per-bucket) byte-for-byte same as
  Phase 1 binary

### Phase 3 (first sweep)

- 80 cells run; OK count ≥ 70/80
- Phase 3 peak per workload archived; comparison line in summary
  with iter-2A-revised values

### Phase 4 (K-channel)

- Hash-diff battery 0 under `FUSEE_K_CHANNELS={1,2,4}` × T ∈ {2,4,8}
  × 5 reps = 45 hash-diff runs
- Smoke at T=4 cache=on shows S3+S4 reduction vs Phase 3 (qualitative)

### Phase 5 (second sweep)

- 80 cells run; OK count ≥ 70/80
- Phase 5 peak per workload ≥ Phase 3 peak on at least 3 of 5 workloads
  (write-heavy workloads should win; read-heavy may be flat)

### Phase 5.5 (K sweep)

- 6-12 cells OK; K-vs-throughput curve produced
- Best K identified with cited supporting data

### Phase 6 (decomp)

- 20 cells; every stage instrumentation reports non-zero
- Sum of (writer stages + sender stages + receiver stages) ≈ wall clock
  per op within 10 % (instrumentation overhead acknowledged)
- iter-4A teaser lists ≥ 3 candidates with evidence

### Phase 7 (summary)

- All index updates landed
- Every Mops/s number in summary cites stage attribution from Phase 6

---

## 7. Success criteria

iter-3A succeeds when ALL of:

1. Cross-host hash-diff battery 0 violations across all phases
   (this is the strict-A correctness witness)
2. 5 sender/receiver probes wired and reporting; iter-2A-revised's
   "sender vs receiver" attribution gap closed with cited data
3. Per-slot LFM ported and verified — S1 reduction at T ≥ 8 visible
   in Phase 6 decomp (qualitative)
4. K-channel implemented and verified — S3+S4 reduction at T ≤ 4
   visible in Phase 6 decomp (qualitative)
5. Two full A-only sweeps complete (Phase 3 + Phase 5), Style B
   30-plot deliverables per scaling_ycsb_spec
6. K_channels parameter sweep produces best-K data (Phase 5.5)
7. Workload A peak Mops/s recorded; if ≥ 5 Mops/s on at least one
   cell, iter-2A-revised's falsified target is now met
8. iter-3A summary doc cites every throughput number with stage
   attribution from decomp (no hand-wave like iter-2A-wire's
   "1.5-2 µs receiver" claim)
9. All deliverables + index updates exist (methodology §7)

**No specific Mops/s target written in this plan** (per CLAUDE.md
§"Time estimates forbidden" generalization to outcome estimates —
quantitative gain is the data, not the target). The qualitative
target is "S1 attacked + S3+S4 attacked + correctness empirically
witnessed."

---

## 8. Risk

| Risk | Likelihood | Mitigation |
|---|---|---|
| Per-slot LFM rehash two-phase lock has subtle correctness bug under strict A (writer A locks slot 3 to delete, writer B locks slot 4 to insert, key races) | Med | C iter-1 already designed two-phase lock; mechanical port. Phase 1 hash-diff battery (5 reps × 3 T) is the witness. |
| K-channel routing collides on cache_epoch_arr (two writers same bucket from different src_hosts via different channels) — impossible by design but defensive `fetch_max` handles | Low | `bucket_id % K` ensures same bucket → same channel everywhere; `fetch_max` is defensive only |
| Phase 1 probe overhead distorts decomp results (5 stages × clock_gettime ≈ 125 ns / op = 0.5 % at 200 K ops) | Low | Compare `FUSEE_LATENCY_DECOMP={0,1}` smoke at T=4; document any > 5 % delta |
| Phase 4 K=2 sender bottleneck shifts elsewhere (e.g., aggregator MPSC contention with K MPSC instead of 1) | Med | Phase 6 decomp will surface this; iter-4A candidate. Mitigation in iter-3A is "data over speculation" — measure first. |
| CPU pinning kicks in but per-channel sender threads still bounce between HT siblings | Med | `sched_setaffinity` to physical core (not HT pair) when possible; verify via `pthread_getaffinity` smoke; document |
| 80-cell sweep at T=84/82/78 changes T grid → not directly comparable to iter-1A T=86 baseline | Acknowledged | iter-3A summary explicitly cites this; archived old T=86 cells linked for reference. Comparison is iter-3A Phase 3 vs Phase 5, not iter-3A vs iter-2A |
| Phase 1 cross-host hash-diff harness reveals iter-2A-revised had quiet correctness violations | Med | **If found**: HALT iter-3A Phase 2-7. (a) Re-run battery 3× to rule out flakiness. (b) If consistent FAIL: write `docs/iters/iter3A_strict_A_correctness_gap.md` documenting which T cells fail + diff sample bytes. (c) Compare which slot fields differ — narrows root cause (slot value race vs cache_epoch race vs writer-vs-receiver ordering). (d) Roll iter-2A-revised summary status forward to "REVERTED + correctness gap discovered post-hoc". (e) iter-3A becomes a correctness-fix iter; pivot scope to repair the strict-A invariant before any optimization work. **This is treated as a discovery, not a project halt** — finding the bug now is far better than shipping a sweep on broken semantics |
| **Methodology violation**: deadline pressure tempts descope of Phase 1 probe instrumentation (treating as "infrastructure", not "data") | **N/A — banned by §1.5 + §6.8** | Phase 1 = required entry condition; cannot ship Phase 6 without Phase 1's probes |

---

## 9. iter-4A teaser (DEFERRED — strict no-commit)

iter-4A direction depends on Phase 6 decomp output. Pre-data
candidates (NOT committed; ranked qualitatively):

a. **Multi-replicator beyond K=4** if Phase 6 shows receiver still
   dominates → keep scaling K
b. **Sender batch-K + T_us re-tune** if Phase 6 shows K_actual
   distribution far from K (e.g., K_actual = 1.5 on average) →
   timeout adjustment may help
c. **B-protocol same N:1:1:N rewire** — same lever as iter-3A
   applied to B
d. **Variable-KV value-size for A** — iter-4 work port to A
e. **Read-path cache=on collapse at high T** (user Q3) — atomic
   acquire-load on hot bucket cache_epoch may be coherence-bound;
   cache_epoch_arr cacheline sharding could help
f. **Cross-host hash-diff harness extension to long-running**
   workload (24-hour soak) — full crash-recovery validation
g. **Solution-2 entry payload compression to 16 B** — was deferred
   from iter-2A; may matter at K=4 high-T where CXL bytes / op
   becomes binding
h. **strict A → eventual A as opt-in mode** — if write-heavy
   workloads still miss 20 Mops/s after iter-3A; provides a
   "performance mode" tradeoff for use cases that don't need
   strict linearizability

---

## Appendix — methodology cross-reference

- `CLAUDE.md` §"Iter execution discipline (HIGHEST CONSTRAINT)" — every phase ships; deadline = full push + autonomous verification; no descope by judgment; **no time estimates in plan**
- `optimization_methodology.md` §1.2 — Phase 1 instrumentation IS the diagnose-before-optimize step (closes iter-2A-revised gap)
- §1.3 — primary hypothesis revision: "S1+S3+S4 dominated → per-slot LFM + K-channel reduces all three" is the hypothesis under test; falsifiable by Phase 6 decomp
- §1.5 — every phase 1..7 ships; spare time → repeat hash-diff battery (e.g., 10 reps), repeat K=4 sweep with 3 reps, add T=86 oversubscription cell as a controlled comparison
- §3.2-§3.3 — Phase 1 reuses `cxl_latency_decomp_A` instrumentation pattern; lock anatomy mirrors C iter-3 phase-1 4-stage but with slot-aware pre-stage
- §4 — iter-3A primary hypothesis stated; falsifiable thresholds are qualitative ("S1 noticeable reduction") because exact targets depend on Phase 1's not-yet-collected baseline
- §6.5 — Phase 2 (per-slot LFM) and Phase 4 (K-channel) split into separate sweep gates (Phase 3 vs Phase 5) so we can attribute gains to each lever
- §6.8 — Phase 1 hash-diff battery + 5 probes mandatory; cannot substitute hand-wave for measurement
- §7 — Phase 7 deliverables checklist
- §9.1 + §9.1.1 — iter-3A's K-channel design is the **fourth** documented Aggregate-before-CXL application (iter-5 V2 + iter-2A wire reverted + iter-2A-revised + this); §9.1.1 corollary 1 (multi-consumer scaling) applied symmetrically on both sender and receiver sides
- `scaling_ycsb_spec.md` §7 — Style B plots mandatory; CPU pinning rule + T-grid adjustment is new — Phase 7 may propose updating the spec to record the convention
