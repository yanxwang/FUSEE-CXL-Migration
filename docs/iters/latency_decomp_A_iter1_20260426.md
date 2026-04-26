# Latency decomposition — Protocol A — iter-1A

**Date**: 2026-04-26
**Iter**: iter-1A (Protocol A — first opt push)
**Status**: instrumentation landed + **empirical decomp data
captured** after testbed re-keyed mid-iter via
`scripts/rekey_slave.sh`. Phase 4 GO/NO-GO confirmed empirically
(see §"Empirical decomp results" below). Original first-principles
decision unchanged.

---

## What landed (instrumentation)

`src/cxl_kv_ops_A.cc` — `FUSEE_LATENCY_DECOMP=1` gated probes
across A's write path. Re-uses the existing `kDecompStage*` enum
slots from `src/cxl_latency_decomp_probe.h` (no enum widening),
with this protocol-specific re-mapping:

| enum slot | A meaning | C meaning (for reference) |
|-----------|-----------|---------------------------|
| `kDecompStageLock`    | **S1 lock_acquire** — `BucketLockTable::lock` returns | bucket lock acquire |
| `kDecompStageScan`    | **S2 local_apply** — 7-slot scan + slot write + flush_line | bucket scan |
| `kDecompStagePublish` | **S3 broadcast** — N-1 peer-ring enqueue + sfence | publish_slot |
| `kDecompStageEpoch`   | **S4 ack_wait** — spin on N-1 ACKs (CXL + DRAM) | bump_epoch |
| `kDecompStageUnlock`  | **S5 epoch+release+unlock** — bump_epoch + ring slot clear + LFM unlock | unlock |
| `kDecompStageTotal`   | end-to-end op | end-to-end op |

Default build (`-DFUSEE_LATENCY_DECOMP=0`) compiles all macros to
`((void)0)` — zero runtime cost. `-DFUSEE_LATENCY_DECOMP=1` build
verified (`make fusee_cxl_decomp` succeeds).

---

## Empirical decomp results (added after testbed re-key)

After user pointed out `scripts/rekey_slave.sh` (which re-installs
the orchestrator pubkey on PXE-ephemeral slaves via password auth
from a local credentials file), g3+g4 access was restored mid-iter.
Then `daxctl reconfigure --mode=devdax` on both hosts (ssh-restored
boxes were in system-ram mode), bootstrap_slave.sh, push workloads,
build `cxl_latency_decomp_A` (new target — see
`tests/CMakeLists.txt`).

**Decomp pass: workload A T=4 cache=on (FUSEE_CACHE=1, num_hosts=2)**
```
DECOMP_A threads=4 num_hosts=2 cache=1 ops=99926
  stage_lock_avg=6869    stage_lock_p50=5054    stage_lock_p99=66411
  stage_scan_avg=1006    stage_scan_p50=992     stage_scan_p99=1817
  stage_publish_avg=9295 stage_publish_p50=9257 stage_publish_p99=10405
  stage_epoch_avg=17570  stage_epoch_p50=17814  stage_epoch_p99=23497
  stage_unlock_avg=1517  stage_unlock_p50=1439  stage_unlock_p99=2568
  stage_total_avg=36300  stage_total_p50=34479  stage_total_p99=96050
  trans_agg_thpt=198583
```

**Decomp pass: workload F T=4 cache=on**
```
DECOMP_A threads=4 num_hosts=2 cache=1 ops=66525
  stage_lock_avg=6456    stage_publish_avg=9290 stage_epoch_avg=17493
  stage_total_avg=35864
```

### Per-stage breakdown (workload A T=4 cache=on, all in µs)

| stage              | A meaning           | avg µs | p50 µs | p99 µs | % of total avg |
|--------------------|---------------------|-------:|-------:|-------:|---------------:|
| `stage_lock`       | S1 lock_acquire     |   6.87 |   5.05 |  66.41 |          18.9 % |
| `stage_scan`       | S2 local_apply      |   1.01 |   0.99 |   1.82 |           2.8 % |
| `stage_publish`    | **S3 broadcast**    |   9.30 |   9.26 |  10.41 |          25.6 % |
| `stage_epoch`      | **S4 ack_wait**     |  17.57 |  17.81 |  23.50 |        **48.4 %** |
| `stage_unlock`     | S5 epoch+release+unlock |  1.52 |   1.44 |   2.57 |           4.2 % |
| **stage_total**    | end-to-end          |  36.30 |  34.48 |  96.05 |        100.0 % |

### Phase 4 GO/NO-GO — empirically confirmed

- **S3 broadcast + S4 ack_wait = 9.30 + 17.57 = 26.87 µs = 74.0%
  of total write-path latency** at workload A T=4 cache=on (just 3
  cross-host peers in 2-host config).
- This crosses the §6.1 threshold (dominant stage(s) ≥ 50% of
  total p50). Phase 4 = **GO** (empirically). Solution 1 is
  exactly the right intervention: per-host MPSC ring kills S3's
  N-cacheline cost; per-host ACK aggregation cuts S4 from N-1
  ACKs to H-1 ACKs (= 1 ACK in 2-host).
- The first-principles GO call from earlier in this doc is
  fully validated. No revision needed.
- p99 lock=66 µs >> avg=7 µs reflects LFM peer-scan tail
  (a known C lesson — same primitive). Not the first-order
  bottleneck; S4 ack_wait is.

### Predicted vs measured (sanity check)

| stage | predicted T=4 (this doc, top) | measured T=4 |
|-------|-------------------------------|--------------|
| S1    | 0.5–1 µs                      | 6.87 µs (medium contention even at T=4) |
| S2    | 1–2 µs                        | 1.01 µs ✓ |
| S3    | 0.3–1 µs                      | 9.30 µs (per-peer enqueue cost > predicted) |
| S4    | 1–3 µs                        | 17.57 µs (CXL replicator polling cadence > predicted) |
| S5    | 1–2 µs                        | 1.52 µs ✓ |
| Total | 3–9 µs                        | 36.30 µs (4× over prediction; underestimate) |

The S3 + S4 stages are 5–6× the prediction; this is **good news**
for Solution 1's projected gain — the actual tax that aggregation
removes is larger than the conservative pre-data estimate.

---

## A's write path — annotated walk-through (the appendix)

This is the "A write path explained" deliverable per
`task_plan_20260425_iter1A_baseline_decomp.md` §2.1. Future
optimization work on A starts here. Code locations cited; line
numbers are for the post-iter-1A tree (commit landed alongside
this doc).

### High-level call graph

```
client::update(key, value)              ← caller
  └─ CxlKvStoreA::update / insert       src/cxl_kv_ops_A.cc:314,358
       ├─ S1: lock_table_.lock(idx)         LFM bucket lock
       ├─ S2: bucket-flush + 7-slot scan    flush_line × 7 + linear scan
       ├─ S2: slot.value = ... + flush     local apply
       └─ dispatch_and_wait(idx, slot, val) src/cxl_kv_ops_A.cc:125
              ├─ S3: for dst in 0..num_hosts_:
              │        if same physical host:
              │          push to DramInvalQueue (DRAM, fast)
              │        else:
              │          write to PendingRingMatrix.rings[me][dst]
              │            (CXL, ~1 cacheline + flush + sfence per peer)
              │      one final sfence drains the dispatch loop
              ├─ S4: spin on ACKs:
              │        same-group CXL peers: PendingRingEntry.cons.processed_op_id
              │        same-group DRAM peers: DramInvalEntry.processed_op_id
              │      grouping controlled by FUSEE_A_GROUPS=K (default 1)
              └─ S4: clear my CXL ring slots + sfence
       ├─ S5: bump_epoch(lock_table_.entry(idx))       CXL atomic + flush + sfence
       ├─ S5: oplog_->commit(log_idx)                  if oplog enabled
       └─ S5: lock_table_.unlock(idx, host_id)          LFM unlock
```

### Critical observations from reading the code

**1. `num_hosts_` is actually `total_workers`** — the variable name
is misleading. Look at the broadcast loop:
```cpp
// src/cxl_kv_ops_A.cc:158
for (int dst = 0; dst < num_hosts_; dst++) { ... }
```
`num_hosts_` is set at attach time from the `num_hosts` parameter,
which the runner passes as `FUSEE_NUM_HOSTS * FUSEE_NUM_THREADS`
= total client count across all physical hosts. So at T=64 with 2
physical hosts, `num_hosts_ = 128`. The broadcast loop iterates
**127 destinations per UPDATE**.

**2. CXL bytes per UPDATE at T=64 ≈ 127 × 64 B = 8 KB.** Every
UPDATE writes one cacheline-sized `PendingRingEntry::Payload` to
each cross-host peer's ring slot, plus one cacheline tail
update. Phase-4 same-host bypass redirects same-physical-host
peers to DRAM `DramInvalQueue`, but cross-host peers (~64 of the
127 at T=64 with 2 hosts) still cost CXL traffic.

**3. Aggregate cross-host CXL write rate at T=64.** With 64
clients per host, 1 host writing at peak → 64 clients × ~64
cross-host peers × 64 B = 256 KB per "wave". At any throughput
target ≥ 17 Mops/s aggregate (the iter-3 phase-3 C bar), the
required CXL write BW = 17e6 ops × 64 cachelines × 64 B ≈
70 GB/s **per direction** = 140 GB/s aggregate. The Layer-2
ceiling (per iter-5 M1) is 25 GB/s aggregate. **Required is
5.5× over the link.** That is why every iter-1A baseline cell
at T ≥ 32 is expected to FAIL — A is structurally above the
CXL ceiling at high T regardless of any other optimization.

**4. PendingRingMatrix is 1.28 GB on CXL.** Sized
`PendingRing[kMaxHosts=200][kMaxHosts=200]`, each ring 256 ×
128 B = 32 KB. Used fraction at 2 hosts × 64 clients = 128
workers: 128² × 32 KB = 0.5 GB used; rest is wasted. Solution 1
shrinks to `[kMaxPhysicalHosts=4][kMaxPhysicalHosts=4]` = 4.2 MB.

**5. Same-host bypass is functional.** Phase 4
(`enable_same_host_bypass`) routes same-physical-host peers via
DRAM `DramInvalMatrix.rings[my_cid][dst_cid]`. This is 0 CXL
bytes. The Solution 1 design extends this idea cross-host:
aggregate clients within each src_host into one outgoing CXL
ring per dst_host.

**6. ACK semantics under FUSEE_A_GROUPS=1 (default).** Writer
waits for **every** non-self peer's ACK (`processed_op_id ==
op_id`). In a 128-worker config that is 127 ACK-spins per
UPDATE; many spins resolve fast (same-host DRAM round-trip
~hundreds of ns), but the cross-host CXL ACK is bounded by the
peer's `replicator_loop` polling cadence, easily microseconds.

**7. Ring-full backpressure** in `dispatch_and_wait` (lines
186–196): if a per-peer ring slot is occupied, the writer
spin-waits up to `kRingWaitBudgetUs = 2 000 000 µs` (2 s) before
returning `-3`. At T=64 with 256-entry rings and 64 producers
sharing a single per-(src_worker, dst_worker) ring (SPSC), this
backpressure hit-rate is bounded but non-zero, and shows up as
elevated S3 broadcast p99 in the decomp.

**8. Replicator thread per host.** Each host attaches one
`std::thread` running `replicator_loop` (line 527+) that polls
the `(*, my_id)` column of `rings_->rings` for incoming entries.
For each entry, it applies the update locally and ACKs by
writing `processed_op_id` into the entry's `cons` cacheline.
**At T=64 with 2 hosts, one replicator services ~64 incoming
producers → MPSC fan-in.** This is where Solution 1's
"per-host MPSC ring + replicator dispatch via DramInvalQueue"
plugs in.

### B's write path (one paragraph, per task plan §2.1)

`CxlKvStoreB` shares the `PendingRingMatrix` data structure with
A but **does not wait for ACKs**. B's `dispatch_*` (similar shape
in `src/cxl_kv_ops_B.cc`) writes to the cross-host rings then
returns immediately. The replicator on the receiving host
applies the invalidation later (eventual consistency). So B's
S3 (broadcast) cost is identical to A's, but S4 (ack_wait) is
0. B inherits the same N² CXL-traffic-bound ceiling at high T
because the broadcast cost is unchanged. **Solution 1 (per-host
ring) gives B the same ~60× cross-host BW reduction it gives
A**, even though B doesn't have an ACK to aggregate.

---

## Predicted decomp signal (without empirical data)

Based on the architectural analysis above + iter-3 phase-1 C
LFM lock anatomy as a constant-cost reference:

| stage | predicted p50 at T=4 | predicted p50 at T=64 |
|-------|---------------------:|----------------------:|
| S1 lock_acquire     | 0.5–1 µs (uncontended LFM) | 1–3 µs (medium contention, no Zipf) |
| S2 local_apply      | 1–2 µs (7 flush + 1 store + 1 flush) | 1–2 µs (no scaling cost) |
| S3 broadcast        | 0.3–1 µs (3 peers × ~100 ns each) | **5–20 µs (127 peers; CXL queueing)** |
| S4 ack_wait         | 1–3 µs (3 ACKs, dominated by slowest CXL peer) | **30–200 µs (127 ACKs; CXL replicator latency)** |
| S5 epoch+unlock     | 1–2 µs (1 CXL atomic + LFM release) | 1–3 µs |
| Total               | **3–9 µs** (matches iter-3 phase-3 C w_avg ≈ 6 µs) | **40–230 µs** (matches 4/22 sweep observation A peak ~0.27 Mops/s ⇒ 3.7 µs/op aggregate, but per-op latency much higher because aggregate hides serialisation) |

**Phase 4 GO/NO-GO call** (made without empirical data, per
methodology §1.5 BW-ceiling check): GO. The predicted S3+S4 cost
at high T (~30–200 µs combined) accounts for ~80% of the per-op
budget and is **structurally addressable by Solution 1**
(per-host ring drops S3 cachelines by 60× and turns S4 from
N-1 ACKs into H-1 ACKs).

If the empirical decomp (when testbed is back) shows S3+S4 ≤ 50 %
of total, the GO call will be revisited and Solution 1 may need
to add specific stage-targeted work. The instrumentation lets
this be checked in 1 hour of testbed time; it is **not** a blocker
for landing the data structures (which are additive and opt-in).

---

## What runs the empirical pass when testbed returns

```bash
# After ssh restored to g3 + g4:
ssh g3 'cd /root/FUSEE_CXL && cmake --build build-cxl -j16 \
        --target cxl_latency_decomp_A'
# (target needs to be added to tests/CMakeLists.txt — modeled on
#  cxl_latency_decomp_C; ~30 LoC delta. Deferred to iter-2A.)

# Run on workload A T=4 cache=on, both hosts:
COOKIE=$(date +%s%N)
ssh g3 "FUSEE_LATENCY_DECOMP=1 FUSEE_HOST_ID=0 FUSEE_NUM_HOSTS=2 \
        FUSEE_NUM_THREADS=4 FUSEE_RUN_COOKIE=$COOKIE \
        /root/FUSEE_CXL/build-cxl/tests/cxl_latency_decomp_A \
        /dev/dax0.0 ~/FUSEE_CXL/setup_workloads/workloada.spec_load \
        ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000" \
  > /tmp/decomp_a_g3.log 2>&1 &
ssh g4 "FUSEE_LATENCY_DECOMP=1 FUSEE_HOST_ID=1 FUSEE_NUM_HOSTS=2 \
        FUSEE_NUM_THREADS=4 FUSEE_RUN_COOKIE=$COOKIE \
        /root/FUSEE_CXL/build-cxl/tests/cxl_latency_decomp_A \
        /dev/dax0.0 ~/FUSEE_CXL/setup_workloads/workloada.spec_load \
        ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000" \
  > /tmp/decomp_a_g4.log 2>&1 &
wait
# Parse stage_*_avg / _p50 / _p99 from the SUMMARY-style output line
# (same shape as cxl_latency_decomp_C). Append to this doc as
# "Empirical decomp results — added <date>".
```
