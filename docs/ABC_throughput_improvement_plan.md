# FUSEE-CXL Protocol A / B / C — Throughput Improvement Plan

Goal: in the current **process-based client model** (no pthread refactor
required), incrementally improve the three protocols' read and write
throughput so that

- **Reads**: A and B match C's scaling shape (both reach ~40-50 Mops/s
  agg at T=86 on workload c/d, vs. today's 3.3 / 3.4 Mops/s)
- **Writes**: A/B/C peak T pushes from T=4-8 out to T=32-64; peak
  aggregate from 1-2 Mops/s to 3-5 Mops/s

5 phases planned. Execution order: **1 → 2 → 4 → 5 → 3**.

Phase 3 (SCHED_FIFO + isolcpus) is **shelved at the end** because it
needs root + reboot + kernel cmdline changes on g3/g4; will be
scheduled when the user has time to coordinate.

All five phases are *additive* and each can be validated independently
with the standard `scaling_ycsb` sweep (see
`docs/scaling_ycsb_spec.md`). None of them **preclude** a future
migration to a pthread client model; in fact Phase 4 **advances** that
migration (see "Process → thread migration impact" at the end).

---

## Phase 1 — A/B read unclamp + read-only attach

**Status**: planned first. No infrastructure prerequisites.
**Effort**: 1-2 days.
**Expected outcome**: A/B workload-c/d at T=86 reach 40-50 Mops/s agg
(C-equivalent). Writes unaffected.

### Motivation

Today A and B are runtime-clamped to `num_clients = 1` per host because
their **write** path shares per-host PendingRing state (one
`local_tail_[dst]` per process, one replicator thread per process). On
pure-read workloads these writes never happen — the read path is
lock-free, no PendingRing touch, purely local. The clamp is
conservative but leaves read scaling on the table.

### Changes

- `src/cxl_kv_ops_{A,B}.{h,cc}`: add an `is_read_only` option to
  `attach(..., init_region, is_read_only = false)`. When set:
  - Skip `replicator_ = std::thread(...)` spawn.
  - Skip `local_tail_[]` initialization.
  - Still attach `lock_table_` and `cache_*_` normally (search uses them).
- `tests/cxl_ycsb_runner.cc`: recognize env `FUSEE_READ_ONLY=1`. When
  set, the parent process still does `attach(init_region=true)` normally
  (it needs the full path to run the load phase as the sole writer).
  **Non-primary fork children** call `attach(init_region=false,
  is_read_only=true)` — skipping their replicators.
- Still require: only client 0 on host 0 runs the load phase (already
  the case after the earlier fix).

### Verification

Run `scripts/run_g34_scaling_sweep.sh` with `OPTS="A B"` and
`WORKLOADS="workloadc"`. Expected plots:

- `A_thpt_workloadc.png` — curve climbs T=1 → T=86 instead of flat.
- `B_thpt_workloadc.png` — same.
- `A/B` p99 latency at high T stays comparable to C.

### Failure mode to watch

Multiple replicator threads on the same host doing nothing during pure
reads is benign. But if any other client ever attempts a write, they
race. Keep the env flag explicit (`FUSEE_READ_ONLY=1`) and assert on
any write attempt when set; fail loud.

### Process → thread migration impact

None. Adding the flag is a pure addition. In a future thread model,
"spawn replicator once per store" stays valid; the read-only bypass is
still a useful configuration.

---

## Phase 2 — LFM → ticket lock

**Status**: planned second. Depends on Phase 1 being in place for
regression testing (read perf should not regress).
**Effort**: 2-3 days.
**Expected outcome**: write workload p99 drops from 45 ms → 10-20 μs;
C peak T for workloads a/f pushes from T=4 to T=16-32; peak thpt
improves 2-3×.

### Motivation

LFM's slow path (`goto retry` after x/y race) degrades badly under
high contention. Observed in the scaling sweep:

- workload-a, C, T=86: p50 write = 10 μs (healthy), p99 write = 45 ms
  (scheduler-tail + LFM spin on preempted holder).
- Ticket lock offers bounded FIFO wait: worst case =
  `(N − 1) × critical_section_time`. For crit ~10 μs and N=172, worst
  case ~1.7 ms — still two orders of magnitude better than LFM's
  observed tail.

### Changes

- New `cxl_shm_profiling/locks/ticket_lock.{h,c}`:
  - `ticket_mutex_t`: `cacheline_u64 next_ticket; cacheline_u64
    now_serving;`
  - `lock`: `my = atomic_fetch_add(&next_ticket, 1); while
    (CACHELINE_LOAD(&now_serving) != my) pause();`
  - `unlock`: `CACHELINE_STORE(&now_serving, my + 1);`
- `src/cxl_bucket_lock.{h,cc}`: add a compile-time / build-time option
  `FUSEE_USE_TICKET_LOCK=ON` to swap `shm_mutex_t` → `ticket_mutex_t`
  inside `BucketLockEntry`. Keep LFM as fallback for comparison and
  for small-N microbenches.
- No change to `CxlKvStore{A,B,C}` — they use `BucketLockTable::lock`
  which stays the same API.

### Risk: CXL atomic cost

On a PCIe-switched CXL memory server, `fetch_add` on shared memory
may be substantially slower than on local DRAM (10-20×) because every
atomic hits the home agent. **Before committing**, run a microbench:

```
cxl_shm_profiling/bench/atomic_fetch_add_cxl_bench /dev/dax0.0 100000
```

Compare ns/op with local DRAM. If CXL `fetch_add` latency > ~5 μs,
ticket lock cost would itself become the tail. Fallback plan: MCS
queue lock (also O(1) worst case but only 2 cachelines per acquire).

### Verification

Re-run `scaling_ycsb` sweep (same command, same workloads). Compare
plots pre/post this phase:

- `C_lat_workloada_write.png`: p99 should drop by 100-1000×.
- `C_thpt_workloada.png`: peak T should shift right; peak thpt should
  climb.
- `A_thpt_workloadc.png` (Phase 1 result): should not regress.

### Process → thread migration impact

None. Ticket lock's state is cacheline-level atomic, lives in CXL
shared memory, behaves identically under either execution model. A
future thread-based client just calls the same `lock(bucket, id,
num_lockers)` API.

---

## Phase 4 — Per-client PendingRing for A/B (unclamp writes)

**Status**: planned third. Depends on Phase 2 so ticket lock can
handle the surge in bucket contention from unclamped A/B.
**Effort**: 1 week.
**Expected outcome**:

- B writes: scale through T=32-64 peak, agg thpt 3-5 Mops/s (today
  240 kops/s clamped).
- A writes: limited by its sync-ACK semantics (every writer waits for
  `total_workers − 1` peers); *doesn't* scale linearly by itself —
  needs Phase 5 to recover.

### Motivation

Today the PendingRing matrix is `rings[kMaxHosts=4][kMaxHosts=4]`,
indexed by cross-host id. Multiple same-host clients both start their
`local_tail_[dst]` at 0 and overwrite each other's slots in
`rings[0][1]`. Also multiple replicator threads on the same host race
on the ring's head cursor.

### Changes

#### 4.1 Widen ring matrix to global-worker indexing

```cpp
// src/cxl_pending_ring.h
constexpr int kMaxWorkers = 256;             // ≥ H × T_max
constexpr int kPendingRingEntries = 256;     // reduced from 4096
struct PendingRingMatrix {
  PendingRing rings[kMaxWorkers][kMaxWorkers];  // [src_gid][dst_gid]
};
```

Memory: 256 × 256 × 256 entries × 6 cachelines × 64 B = **12 GiB**.
Fits in the 512 GiB devdax region. Ring depth reduced from 4096 to
256 because each (src,dst) pair carries far fewer concurrent in-flight
ops now.

#### 4.2 Writer path → global_id indexing

```cpp
int CxlKvStoreA::dispatch_and_wait(...) {
  for (int peer_gid = 0; peer_gid < total_workers_; peer_gid++) {
    if (peer_gid == my_gid_) continue;
    PendingRing *ring = &rings_->rings[my_gid_][peer_gid];
    // local_tail_[peer_gid] is per-process, per-(src,dst) — still OK
    //   because each (src,dst) ring still has exactly ONE producer.
    ...
  }
}
```

`my_gid_` is stored in `CxlKvStoreA` at attach time (new field). For
attach call: pass `global_id = host_id × num_clients + client_id`
(runner already computes this).

#### 4.3 Replicator per client

Each fork child spawns its own replicator thread, consuming
`rings[*][my_gid_]`. Each (src, dst) ring has exactly one consumer
(the dst client's replicator). True SPSC.

#### 4.4 Runner: remove A/B clamp for mixed workloads

Once 4.1-4.3 land, the clamp becomes unnecessary. `FUSEE_NUM_THREADS`
is honored for all three protocols.

### Constraint on A

A's writer waits for *all* peers' ACKs. With per-client rings and
`total_workers = 172`, each write now waits on 171 peers — latency
explodes, throughput plummets. **A does not benefit from Phase 4
alone.** Phase 5 addresses this.

### B expected gain

B's writer pushes without waiting. Per-client rings let N concurrent
writers on a host each push to their own ring; each receiver's
replicator consumes its own `rings[*][self]` subset. No producer or
consumer contention between clients within the same host. Expected
agg thpt 3-5 Mops/s at T=32-64.

### Verification

Run `scaling_ycsb` sweep with all workloads, all three opts. Compare:

- `A_thpt_workloada.png`: still flat-ish (before Phase 5).
- `B_thpt_workloada.png`: climb through T=32-64.
- `C_thpt_workloada.png`: should not regress (Phase 2 ticket lock
  continues to help here).

### Process → thread migration impact

**Positive — Phase 4 is a step toward thread model.** Phase 4's
`global_id`-indexed ring matrix is exactly the addressing scheme a
thread-based model would use. In thread mode, same-process threads
could additionally skip CXL rings for intra-process communication
(use DRAM queues instead), a pure optimization — Phase 4's design
stays correct. The memory overhead in thread mode is ~75 % "wasted"
(intra-process pairs still have a CXL ring allocated); a one-line
lazy-alloc change recovers that.

---

## Phase 5 — Hierarchical replication for A

**Status**: planned fourth. Depends on Phase 4 (needs per-client
rings to exist).
**Effort**: 1-1.5 weeks.
**Expected outcome**: A writes scale similarly to B, peak 3-5 Mops/s
at T=32-64.

### Motivation

A's semantics — sync with *all* peers — is fundamentally O(N) per
write. Even with per-client rings, at N=172 every write waits on 171
peer ACKs before returning. Throughput upper bound is roughly
`1 / (avg peer ACK latency)` per writer; aggregate is
`total_workers / (num_peers × ACK latency)` which *decreases* with N
(harmonic).

### Changes

#### 5.1 Group topology

Partition `total_workers` clients into `K` groups of roughly equal
size (K chosen empirically; 4 groups of ~43 clients or 8 groups of
~22 is the starting point). Group membership is fixed at attach time
(all clients know each other's group id via shared-CXL config block).

#### 5.2 Replication rule

- **Within group**: sync — writer waits for every same-group peer's
  ACK. `num_peers_within = N/K − 1` (e.g., 42 at K=4).
- **Across group**: eager push (B-style) — writer pushes into other
  groups' ring but doesn't wait.
- Each client still consumes `rings[*][self_gid]`. No per-group
  replicator needed — same client-level replicator model as Phase 4.

#### 5.3 Consistency implications

- *Intra-group*: linearizable writes (strong consistency).
- *Cross-group*: eventually consistent. A read served by a client in
  group G may miss writes that originated in another group but haven't
  propagated yet.

Workload placement needs to respect this — share the same key range
within one group for RYW semantics. For a pure hash-partitioned
workload (Zipfian over all keys), this is fine because keys naturally
hash into buckets that all clients access; the inconsistency window
is bounded by cross-group ring propagation latency (tens of μs on CXL).

#### 5.4 Config surface

`FUSEE_A_GROUPS=K` env (default 1 = current all-sync behavior, so the
protocol change is opt-in). Group assignment is round-robin:
`my_group = my_gid_ % K`.

### Verification

Run `scaling_ycsb` with `FUSEE_A_GROUPS=4` and `=8`. Compare
`A_thpt_workloada.png` / `workloadf.png` to Phase 4 baseline.

### Process → thread migration impact

None. Consistency protocol is orthogonal to execution unit.

---

## Phase 3 — SCHED_FIFO + isolcpus (shelved)

**Status**: **deferred** pending user coordination for
root-level kernel cmdline changes and host reboots on g3/g4.
**Effort**: 0.5 day once access is arranged.
**Expected outcome**: all workloads' p99 drops further 2-3×;
aggregate throughput on write-heavy workloads +20-30 %.

### Motivation

At T=86, each host has 86 clients on 86 cores (1:1 mapping); tail
comes from system processes (kworker, IRQ handlers, systemd-*)
stealing cycles on shared cores. Any client whose core is preempted
for 10 ms stalls every other client waiting on its bucket lock via the
shared CXL memory.

### Changes

#### Host-side (needs root + reboot)

Edit `/etc/default/grub` on g3 and g4:
```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=4-85 nohz_full=4-85
                            rcu_nocbs=4-85"
```
`update-grub && reboot`. Cores 4-85 become "isolated" — kernel
scheduler won't put any regular user/system tasks on them unless
explicitly bound. Core 0-3 handle IRQ, systemd, sshd, etc.

#### Runner-side

`scripts/run_g34_scaling_sweep.sh` wraps each client ssh invocation:

```
chrt -f 10 taskset -c 4-$((4 + NUM_CLIENTS - 1)) \
    $cmd_base
```

`chrt -f 10` = SCHED_FIFO priority 10. `taskset -c ...` pins each
client to a specific isolated core.

### Verification

Re-run `scaling_ycsb`. Compare p99 latency plots pre/post.

### Why shelved

The GRUB change requires a full reboot of g3 and g4, and PXE-reboot
concerns (will `isolcpus=4-85` survive a PXE re-image? probably yes,
but needs testing). User said to defer until a scheduled window is
available.

---

## Timeline summary

| order | phase | depends on | effort | status |
|-------|-------|-----------|--------|--------|
| 1 | Phase 1 — A/B read unclamp | — | 1-2 days | next |
| 2 | Phase 2 — ticket lock | Phase 1 (regression test) | 2-3 days | after 1 |
| 3 | Phase 4 — per-client ring | Phase 2 | 1 week | after 2 |
| 4 | Phase 5 — hierarchical A | Phase 4 | 1-1.5 weeks | after 4 |
| — | Phase 3 — SCHED_FIFO | user-coordinated reboot window | 0.5 day | shelved |

Total Phase 1+2+4+5 ≈ 3 weeks of focused work. Phase 3 is a 0.5-day
slot whenever it gets scheduled.

## How each phase is validated

All phases use the same validation flow: re-run the **standard
`scaling_ycsb` experiment** (see `docs/scaling_ycsb_spec.md`) and
compare the 30 main plots + 10 abc_compare plots to the previous
phase's baseline. Each phase's own section lists which specific plots
are expected to change and in what direction.

## Process → thread migration: aggregate impact

If after all phases we later decide to migrate to a pthread client
model, the required work is:

1. Move `CxlKvStore{A,B,C}`'s per-instance fields (`my_gid_`,
   `num_hosts_`, `local_tail_`) to `thread_local` storage.
2. Attach once per process, each thread calls insert/update/search
   with its own `my_gid_` passed via thread-local.
3. For intra-process pairs, optionally swap CXL ring → in-DRAM queue
   (optimization, not required).

Estimated effort: **2-3 days** (down from ~1 week if we hadn't done
Phase 4's `global_id` refactor). So the 5 phases not only don't
obstruct a future thread migration — they reduce its cost by ~60 %.
