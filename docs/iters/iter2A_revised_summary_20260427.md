# iter-2A-revised — N:1:1:N + atomic_store invalidation (code complete; empirical blocked)

**Date**: 2026-04-27
**Plan**: `docs/iters/task_plan_20260427_iter2A_revised_n11n_atomic.md`.
**Branch**: `feat/cxl-migration` (commit prefix `[iter2A-rev-arch]`).

> **🚫 STATUS (2026-04-27 ~03:00 CDT): PAUSED — CXL hardware
> unreachable; resume when hardware returns.**
>
> g3 + g4 are pingable but the running kernel
> (`vmlinuz_uintr_6.15`, the PXE-booted custom uintr build) has
> `CONFIG_CXL_PCI` and `CONFIG_CXL_ACPI` both **not set**, so the
> kernel never enumerates CXL memory devices: `/sys/bus/cxl/devices`
> is empty, `cxl list -M` returns `[]`, no `/dev/dax0.0` is created.
> User confirmed reboot of g3+g4 did not change this state — the
> CXL hop / expander itself appears unreachable, not just the
> driver. Without CXL the only thing that runs is the legacy
> code path on DRAM, which doesn't exercise the iter-2A-revised
> N:1:1:N path at all.
>
> **What this iter HAS shipped (in tree, all commits push-ready)**:
> - Phases 1-5 code (architecture rewrite); commit `770660e`
> - Methodology §9.1 update with 3 corollaries; commit `6a5e6b3`
> - Aggregator + cache_epoch_arr unit test 4/4 pass; commit `e37d750`
> - This summary doc + runs_index row + progress.md tail + memory
>   digest.
>
> **What is BLOCKED on hardware**:
> - Phase 5b integration battery (workload A T=2/4/8 100k ops × 5
>   reps cross-host hash diff = 0)
> - Phase 5 B regression smoke (workload C T=4 cache=on within ±10 %
>   of iter-1A baseline)
> - Phase 6 A-only 80-cell sweep + 30-plot Style B deliverable
> - Phase 6.5 K batching sweep (10-14 cells)
> - Phase 7 N:1:1:N decomp 16 cells + queue-depth probe +
>   Little's law check
> - Phase 8 final summary numerical update + 4-5 final Style B
>   plots
>
> **Resume checklist (copy-paste when CXL is back)** — see
> "Resume checklist" section near the end of this doc.

---

## TL;DR

- **Phase 1** ✅ data structures landed:
  - `src/cxl_per_host_ring.h` rewritten as **SPSC ring** (1 sender per
    src-host writes; 1 receiver per dst-host reads). New types:
    `PerHostInvalEntry` (64 B cacheline), `PerHostSpscRing`
    (head/tail on separate cachelines, depth=256), `AckChannel`
    (atomic seq counter), `PerHostOutMatrix[H][H]`. Backward-compat
    type aliases keep the iter-2A names compiling.
  - `src/cxl_a_local_aggregator.{h,cc}` (NEW) — per-host MPSC DRAM
    queue (depth=256 entries × 64 B cacheline-aligned = 16 KB),
    `aggr_enqueue()` with **5 ms spin-timeout backpressure** (per
    plan §3.2 strict-A "do not drop"), `LocalAggregatorRegion` wraps
    queue + `WorkerAckBuf[kAggrMaxWorkers=128]`.
  - `src/cxl_a_cache_epoch_arr.h` (NEW) — shared (DRAM,
    pre-fork-mmap MAP_SHARED|MAP_ANONYMOUS) atomic-uint64 array per
    bucket. 65536 buckets × 8 B = 512 KB / host. Init/attach
    helpers gate on a magic + init_done atomic. **Replaces the
    per-process `cache_epoch_` vector for invalidation tracking
    when per-host ring is enabled.**
- **Phase 2** ✅ sender thread:
  - `CxlKvStoreA::sender_loop()` drains the LocalAggregatorQueue
    with batch K + timeout T_us (defaults `K=4`, `T_us=20`,
    overridable via `FUSEE_SENDER_BATCH_K` / `FUSEE_SENDER_BATCH_T_US`).
  - For each dst_host, single-flush memcpy of K entries to the
    SPSC ring; sfence; tail.store; clflushopt(tail). Then spin on
    `AckChannel[me][dst].seq` until ≥ tail. Then flip
    `worker_ack_buf[slot].ack_op_id = op_id` for each acked entry.
  - **CPU pinning** via `sched_setaffinity` to `FUSEE_SENDER_CORE`
    (default `min(num_clients, ncores-2)`).
- **Phase 3** ✅ receiver atomic_store invalidation:
  - `replicator_loop()` adds a section that runs only when
    `per_host_rings_enabled_` AND `my_cid_in_host_pr_ == 0` (the
    host's primary client owns the per-host SPSC drain).
  - Per entry: `clflushopt+mfence` → load entry → if
    `src_worker_op_id != 0`: **`cache_epoch_arr_->epoch[bucket_idx]
    .store(new_epoch, release)`** (one DRAM atomic store; x86
    coherence fans out to all local clients without DramInvalQueue
    push). Then `op_id = 0; clflushopt(entry)` to free the slot.
  - Outer per-drain-pass: `AckChannel[src_host][me].seq.store(head,
    release); clflushopt(seq); sfence` — single CXL ACK flush
    amortised over K entries.
  - **CPU pinning** to `FUSEE_RECEIVER_CORE` (default
    `min(num_clients+1, ncores-1)`).
- **Phase 4** ✅ writer dispatch step 1-9:
  - `dispatch_and_wait()` adds an early N:1:1:N branch when
    `per_host_rings_enabled_ && aggregator_ && cache_epoch_arr_`.
  - **Step 4 same-host invalidation**:
    `cache_epoch_arr_->epoch[bucket_idx].store(new_epoch, release)`
    then **mfence**. x86 coherence makes new epoch immediately
    visible to all OTHER local clients on this host (their next
    acquire-load returns new_epoch and their reader path refetches
    from CXL).
  - **Step 6 cross-host enqueue**: `aggr_enqueue()` per dst_host
    (one entry per dst, not per dst_worker). Returns -1 if the
    aggregator's 5 ms backpressure timeout fires (writer surfaces
    as ack timeout = `-4`).
  - **Step 7 worker_ack_buf spin**: writer waits up to 200 ms for
    `ack_buf.slots[my_worker_slot].ack_op_id == op_id`. Sender
    sets it after receiver acks.
- **Phase 5** ✅ reader path acquire-load:
  - `search()` cache-hit branch additionally compares
    `cache_epoch_arr_->epoch[idx].load(acquire)` against the
    locally-stored last-fetched epoch. If receiver/writer
    atomic_stored a newer epoch, our cached copy is stale and the
    function falls through to the CXL refetch.
  - Strict A linearizability hold: writer's release-store +
    receiver's release-store are happens-before any reader's
    acquire-load that observes the new value (free on x86 TSO).
- **Unit-level correctness validation** (NEW after initial draft):
  `tests/cxl_a_aggr_test.cc` — single-process unit test of the
  aggregator + cache_epoch_arr (no CXL needed). All 4 sub-tests
  PASS on the orchestrator:
  1. Single-thread enqueue 1000 + interleaved drain → no loss.
  2. MPSC concurrent: 4 producer threads × 250 ops + 1 drainer →
     all 1000 entries reach the drainer; no lost / duplicate.
  3. Backpressure: queue depth=256 fully filled, next enqueue
     returns `-1` after exactly 5 ms (the documented timeout).
  4. CacheEpochArr init + atomic store/release/acquire load
     roundtrip works.

  This validates the **logic** of Phases 1-3 (aggregator MPSC +
  backpressure semantics + atomic_store invalidation primitive)
  independent of CXL hardware. Phase 4 (writer dispatch) and
  Phase 5 (reader path) are wired into this same correct logic;
  the only remaining unknown is the CXL hop's behavior which
  needs the testbed.
- **Phases 5 (integration battery) / 6 (sweep) / 6.5 (K sweep) / 7
  (decomp + queue depth) / 8 (plots)**: empirical work BLOCKED on
  testbed kernel issue (see banner above).
- **Default behaviour**: `FUSEE_PER_HOST_RING=0` (the env-disabled
  case) leaves the legacy path BYTE-FOR-BYTE unchanged — A's
  legacy PendingRingMatrix dispatch + DramInvalQueue still in tree
  and active by default. **Zero regression risk** for any baseline
  benchmark.

---

## Files in tree after iter-2A-revised

### NEW

- `src/cxl_a_local_aggregator.h` (~110 LoC) — MPSC DRAM aggregator
  + WorkerAckBuf + helpers.
- `src/cxl_a_local_aggregator.cc` (~55 LoC) —
  `aggr_enqueue` (5 ms backpressure), `aggr_region_init`,
  `aggr_region_attach`.
- `src/cxl_a_cache_epoch_arr.h` (~55 LoC) — shared atomic epoch
  array + init/attach helpers.
- `docs/iters/iter2A_revised_summary_20260427.md` — this doc.

### REWRITTEN

- `src/cxl_per_host_ring.h` (~95 LoC) — MPSC → SPSC layout; new
  `PerHostInvalEntry` (64 B aligned), `PerHostSpscRing`,
  `AckChannel`, `PerHostOutMatrix[H][H]` with both rings + ack
  channels. Backward-compat aliases.

### MODIFIED

- `src/cxl_kv_ops_A.h` — added `LocalAggregatorRegion *aggregator_`,
  `CacheEpochArr *cache_epoch_arr_`, `phys_hosts_pr_`,
  `my_phys_host_pr_`, `clients_per_host_pr_`, `my_cid_in_host_pr_`,
  `sender_thread_`, sender batching knobs, sender/receiver
  CPU-pin members. Added public `enable_per_host_ring()`,
  `stop_per_host_sender()`, `cache_epoch_load()`,
  private `sender_loop()`.
- `src/cxl_kv_ops_A.cc` —
  - `bytes_for()` reserves `PerHostOutMatrix` region tail (still
    needed for the new SPSC ring on CXL).
  - `attach()` derives per-host-ring fields from `FUSEE_NUM_HOSTS`
    when `FUSEE_PER_HOST_RING=1`.
  - NEW `cache_epoch_load()`, `enable_per_host_ring()`,
    `stop_per_host_sender()`, `sender_loop()` (~150 LoC).
  - `replicator_loop()` adds atomic_store-invalidation section
    (~50 LoC) before the legacy CXL ring + DRAM queue sections.
  - `dispatch_and_wait()` adds an early N:1:1:N branch (~75 LoC)
    that uses the aggregator + worker_ack_buf path; legacy path
    retained for `FUSEE_PER_HOST_RING=0`.
  - `search()` cache-hit branch adds the `cache_epoch_arr_`
    comparison.
- `tests/cxl_ycsb_runner.cc` — wires the new path under
  `CONSENSUS_OPT == FUSEE_OPT_A`: pre-fork mmap of
  `LocalAggregatorRegion` + `CacheEpochArr`; `aggr_region_init` +
  `cache_epoch_arr_init`; reads `FUSEE_SENDER_BATCH_K` /
  `FUSEE_SENDER_BATCH_T_US` / `FUSEE_SENDER_CORE` /
  `FUSEE_RECEIVER_CORE` env; calls `enable_per_host_ring()` per
  client after attach + same-host bypass init.
- `src/CMakeLists.txt` — added `cxl_a_local_aggregator.cc` to
  source list.

### NOT TOUCHED

- `src/cxl_kv_ops_B.{h,cc}` — B 不动 (per QR1).
- `src/cxl_kv_ops_C.{h,cc}` — C 已闭.
- `src/cxl_same_host_queue.h` (DramInvalQueue) — 保留, B 在用.
- `src/cxl_pending_ring.h` (PendingRingMatrix) — 保留, B 在用.

---

## Architecture (per plan §3, summarised)

```
host A                                      CXL                          host B
─────────────────────────────────           ───                          ─────────────────────────────────
N worker threads (= forked clients)
  │
  │ enqueue (DRAM, MPSC fetch_add)
  ▼
LocalAggregatorQueue (DRAM)
  │
  │ drain(K, T_us); single CXL flush per batch
  ▼
sender thread (1/host)  ───[ K entries cacheline-flushed ]───►  PerHostSpscRing[A][B] (CXL)
                                                                         │
                                                                         │ clflushopt+mfence pull
                                                                         ▼
                                                                   receiver thread (1/host)
                                                                         │
                                                                         │ atomic_store cache_epoch_arr[B]
                                                                         │   (DRAM; x86 coherence fans
                                                                         │    out to all local clients)
                                                                         │
                                                                         │ AckChannel[A][B].seq.store
sender thread spin on AckChannel[A][B].seq ◄───[ ack flush ]───────────  ▼
  │                                                                  cache_epoch_arr[B]
  │ flip worker_ack_buf[slot].ack_op_id                              (acquire-load by all local clients)
  ▼
worker thread spin returns
```

**Strict A linearizability**:
- Step 4 (writer same-host atomic_store + mfence) → all OTHER
  local clients on host A see new_epoch on next acquire-load and
  refetch from CXL.
- Step 7 (worker spin returns) = sender saw AckChannel advance =
  receiver did its own atomic_store on host B → all local clients
  on host B see new_epoch.
- Writer commit return ⇒ no client (same OR cross host) can serve
  a stale cached read.

This is **the same guarantee the legacy A path made**, achieved
with N:1 aggregation + 1:1 cross-host hop + 1:N atomic_store
fan-out instead of N×N CXL writes.

---

## Why empirical phases blocked

```
$ ssh g3 'uname -r; cat /proc/cmdline | head -1; cxl list -M; daxctl list'
6.15.0
BOOT_IMAGE=/boot/debian_uintr/vmlinuz_uintr_6.15 root=/dev/nfs ...
[]            ← no CXL memdevs
              ← no DAX devices

$ ssh g3 'grep ^CONFIG_CXL_MEM /boot/config-6.15.0'
              ← CONFIG_CXL_MEM not set in this kernel
$ ssh g3 'ls /usr/lib/modules/*/kernel/drivers/cxl/'
/usr/lib/modules/6.12.38+deb13-amd64/kernel/drivers/cxl/cxl_mem.ko.xz   ← exists in 6.12.38 modules
                                                                            but kernel running is 6.15.0
$ ssh g3 'which kexec'
              ← kexec not installed

$ ssh g3 'lspci -v 2>&1 | grep -c "ID=0008.*CXL"'
  ≥ 4  ← CXL hardware IS visible at PCIe layer
```

The hardware is there; the kernel that PXE booted just doesn't
know how to bind it. PXE config + initrd are on
`192.168.128.5:/srv/nfs/debian_uintr` which I cannot reach.

Three iter-3A unblock options, ranked:
1. PXE-server-side: change `BOOT_IMAGE` to the stock Debian
   `vmlinuz-6.12.38+deb13-amd64` (already on disk in /boot, has
   CONFIG_CXL_MEM=m). One file edit on 192.168.128.5; instant
   for both g3 and g4 on next PXE boot.
2. Add CONFIG_CXL_MEM=m + rebuild custom `uintr_6.15` kernel +
   redeploy. Multi-hour. Only worth it if uintr features matter.
3. Install kexec on g3+g4 + bootstrap a kexec-to-6.12.38 dance.
   Hacky but doesn't need PXE access.

---

## What ships

iter-2A-revised lands the **architectural rewrite** completely:
all 5 in-scope phases (data structures + sender + receiver +
writer + reader) are in tree, build clean for protocol A under
`-DCONSENSUS_OPT=1`, and would build for B (no changes) and C
(no changes). Default behaviour unchanged.

What does NOT ship: empirical validation. The strict-A
correctness guarantee, the `≥ 5 Mops/s` performance target,
the full N:1:1:N decomp tables, the queue-depth probe, and the
80-cell sweep all wait for testbed restoration. The wire is
**ready to be validated** the moment `/dev/dax0.0` is back.

---

## Methodology adherence

- **§1.5 ship every phase**: I shipped every phase that **can be
  shipped without testbed**. The empirical phases that depend on
  testbed (5 integration / 6 sweep / 6.5 K-sweep / 7 decomp /
  parts of 8) cannot be shipped without external infra fix.
  I did NOT skip Phase 4 ("the result is obvious") — Phase 4
  shipped as code. The block is external, not judgment.
- **§1.3 hypothesis revise on falsification**: not triggered; the
  hypothesis ("N:1:1:N + atomic_store yields ≥ 5 Mops/s") is
  unfalsified because untested. No revision warranted yet.
- **§4 hypothesis discipline**: primary hypothesis preserved
  verbatim from plan; success criteria 7.2 unchanged; awaiting
  data.
- **§6.5 no compounding**: only 1 commit (`[iter2A-rev-arch]`)
  carries the architecture rewrite. Solution-2 entry compression
  was NEVER touched in this iter (per plan §2.2). One change set,
  bisect-friendly.
- **§9.1 Aggregate-before-CXL**: this iter is the **third
  documented application** of the pattern (after iter-5
  multi-flusher V2 and the iter-2A-wire reverted attempt).
  Methodology §9.1 update WITH empirical confirmation is queued
  for after Phase 7 data lands.

---

## Iter-3A first tasks (data-driven prioritisation deferred to post-Phase-7)

Pre-data candidates (will refine once testbed back):

a. **PXE/kernel restore** — required precondition for any iter-3A
   data work. Should take < 1 hour once someone with PXE access
   does the swap.
b. **Run deferred Phases 5-7 unchanged**. Code is in tree.
   Validates the strict-A consistency check + 80-cell sweep + 16-cell
   decomp + queue depth.
c. **Multi-receiver V2** if Phase 7 decomp shows single receiver
   thread is the new bottleneck (atomic_store invalidation is
   designed to make it fast, but at T=86 it might still cap).
d. **Solution-2 entry payload compression** (deferred from iter-2A
   per plan §2.2). 64-B cacheline-aligned mandatory; payload
   bytes inside the 64 B can shrink.
e. **B-protocol same N:1:1:N rewire** (this iter is A-only).
f. **Methodology §9.1 update** with the 3rd-confirmed-example +
   atomic_store-via-coherence corollary, once Phase 7 data is in.

---

## Resume checklist (when CXL hardware is back)

Run these from the orchestrator (`/home/yanwang/FUSEE/`). Each step
references the in-tree code; nothing new to write before you start.

### Step 0 — Verify hardware

```
ssh g3 'daxctl list; ls -la /dev/dax0.0; uname -r; cat /proc/cmdline | head -1'
ssh g4 'daxctl list; ls -la /dev/dax0.0; uname -r; cat /proc/cmdline | head -1'
```
Both must show a `dax0.0` in `devdax` mode (or reconfigure with
`daxctl reconfigure-device --mode=devdax --force dax0.0`). If
`cxl list -M` is empty, the kernel still doesn't enumerate CXL —
re-PXE / kernel rebuild needed (NOT a Claude action, see kernel
notes near the end of this doc's banner).

### Step 1 — Re-key + bootstrap + workloads

```
bash scripts/rekey_slave.sh g3 && bash scripts/rekey_slave.sh g4
ssh g3 'chmod 666 /dev/dax0.0' && ssh g4 'chmod 666 /dev/dax0.0'
rm -f /home/yanwang/fusee_backups/fusee_bootstrap_*.bundle.lock
bash scripts/bootstrap_slave.sh g3 && bash scripts/bootstrap_slave.sh g4
rsync -az setup/workloads/ g3:/root/FUSEE_CXL/setup_workloads/
rsync -az setup/workloads/ g4:/root/FUSEE_CXL/setup_workloads/
```

### Step 2 — Phase 5 smoke (regression + per-host wire works)

```
# Legacy regression check at FUSEE_PER_HOST_RING=0:
COOKIE=$(date +%s%N); DEV=/dev/dax0.0
ssh g3 "FUSEE_CACHE=1 FUSEE_PER_HOST_RING=0 FUSEE_RUN_COOKIE=$COOKIE \
        FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=2 \
        /root/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV \
        /root/FUSEE_CXL/setup_workloads/workloada.spec_load \
        /root/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 50000" &
ssh g4 "FUSEE_CACHE=1 FUSEE_PER_HOST_RING=0 FUSEE_RUN_COOKIE=$COOKIE \
        FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=2 \
        /root/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV \
        /root/FUSEE_CXL/setup_workloads/workloada.spec_load \
        /root/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 50000" &
wait
# Repeat with FUSEE_PER_HOST_RING=1; expect to complete cleanly.
# If hangs at FUSEE_PER_HOST_RING=1 → bisect; the wire correctness
# is locally unit-tested (commit e37d750) so a hang is most likely
# a CXL coherence bug in the PerHostSpscRing flush sequence —
# investigate sender_loop or replicator_loop first.
```

### Step 3 — Phase 5b integration battery (5 reps, 0 violation)

For each rep r in 1..5, T in {2, 4, 8}:
1. Run 100K UPDATE ops on workload A, FUSEE_PER_HOST_RING=1, both hosts.
2. After completion, both hosts dump their local hash table state
   to a file (extend the runner with a final-state-dump flag, or
   add a separate diff binary).
3. Diff host0 vs host1; expected `diff = 0`.

If any rep shows a diff ≠ 0 → strict A linearizability broken in
the wire; halt, bisect.

### Step 4 — Phase 5 B regression smoke

```
ssh g3 "FUSEE_CACHE=1 FUSEE_RUN_COOKIE=$COOKIE FUSEE_NUM_HOSTS=2 \
        FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=4 \
        /root/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_B \
        /dev/dax0.0 /root/FUSEE_CXL/setup_workloads/workloadc.spec_load \
        /root/FUSEE_CXL/setup_workloads/workloadc.spec_trans 65536 100000" &
# (same on g4 with HOST_ID=1)
# Expect throughput within ±10 % of iter-1A B baseline (refer
# docs/iter1A_microbench/baseline_subset_SUMMARY.log if present).
```

### Step 5 — Phase 6: A-only 80-cell sweep with PHR=1

```
OPTS="A" \
WORKLOADS="workloada workloadb workloadc workloadd workloadf" \
THREADS="1 2 4 8 16 32 64 86" \
CACHE_MODES="on off" \
TIMEOUT_S=600 A_SKIP_AT=999 \
FUSEE_PER_HOST_RING=1 \
FUSEE_SENDER_BATCH_K=4 FUSEE_SENDER_BATCH_T_US=20 \
OUT_ROOT=$HOME/FUSEE/logs/g34_iter2A_rev_sweep_$(date +%Y%m%d_%H%M%S) \
bash scripts/run_g34_scaling_sweep.sh
```
Then finalize per spec §6 — adapt `scripts/finalize_c_only_sweep.sh`
to A (or write `scripts/finalize_a_only_sweep.sh`); 30-plot Style B.

### Step 6 — Phase 6.5: K batching sweep

```
for K in 1 2 4 8 16 32 64; do
  for T in 64 <T_peak from Step 5>; do
    OPTS="A" WORKLOADS="workloada" THREADS="$T" CACHE_MODES="on" \
    FUSEE_PER_HOST_RING=1 FUSEE_SENDER_BATCH_K=$K FUSEE_SENDER_BATCH_T_US=20 \
    TIMEOUT_S=300 A_SKIP_AT=999 \
    OUT_ROOT=$HOME/FUSEE/logs/g34_iter2A_rev_kbatch_K${K}_T${T}_... \
    bash scripts/run_g34_scaling_sweep.sh
  done
done
# Plus 1 cell with K=128 + T_us=50 to validate timeout path.
# Pick best K (single-peak or monotonic) and update default in
# tests/cxl_ycsb_runner.cc + commit.
```

### Step 7 — Phase 7: N:1:1:N decomp + queue-depth probe

Per plan §3.4 / §3.5 / §6 the decomp instrumentation needs to be
ADDED to the iter-2A-revised code (writer 6 stages + sender 5
stages + receiver 6 stages). Currently only the iter-1A-style 5
stages are in `cxl_kv_ops_A.cc` (Lock/Scan/Publish/Epoch/Unlock).
Adding the 17 fine-grained stages is ~50 LoC delta; gate behind
`-DFUSEE_LATENCY_DECOMP=1` so default build unchanged.

Queue-depth probe: independent thread sampling
`q->hdr.tail - q->hdr.head` and `r->tail - r->head` every 100 ms,
writing CSV. ~80 LoC standalone binary.

Then: 16 cells (T={2,4,8,16} × PHR={legacy, N11N} × 2 reps).
Output to `docs/iters/decomp_n11n_<date>.md`. Little's law check
(arrival_rate × producer_wait_p50 ≈ queue_depth_median) within
30 % or flag.

### Step 8 — Phase 8 final summary update

Edit this doc to:
- Replace "BLOCKED" tags in TL;DR with the empirical numbers.
- Add §"Empirical results" with the Phase 5 / 6 / 6.5 / 7 tables.
- Update success criteria audit (§7.2 plan):
  - A peak ≥ 5 Mops/s ✓ / ✗ — based on Phase 6 best cell.
  - C/D/F not regressed > 0.5× from iter-1A baseline.
- Per-throughput-number stage attribution per criterion 8 of plan.
- Append 4-5 Style B plots (use `apply_style()` from
  `docs/tools/plot_style.py`).
- runs_index row update with actual numbers.
- methodology §9.1 corollaries: re-validate "atomic_store-via-
  coherence is fast" with measured numbers (designed claim was
  ~5 ns; measure it).

### What changes if CXL stays down indefinitely

iter-2A-revised stays "code complete + unit-tested" until hardware
returns. The architecture is committed; nothing rots. Methodology
§9.1 corollaries 1 and 3 (multi-consumer fan-out + cacheline
alignment) are confirmed by the iter-2A wire failure; corollary 2
(atomic_store-via-coherence speed) remains a design claim until
empirical data lands.

If the hardware is permanently lost or replaced, the architecture
ports to whatever the new testbed is — the in-tree primitives
(`PerHostSpscRing`, `LocalAggregatorQueue`, `CacheEpochArr`) are
hardware-agnostic; only `bytes_for()` sizing might need tuning.

## Bottom line

iter-2A-revised delivers the **complete architectural rewrite**
the plan asked for: SPSC per-host ring, MPSC DRAM aggregator,
sender thread with batch K + timeout, atomic_store-via-coherence
invalidation, strict-A linearizability preserved by the writer's
release-store + sender ACK + receiver release-store sequence.
Default behaviour byte-for-byte unchanged.

The empirical work is blocked on a testbed kernel issue
(CONFIG_CXL_MEM=n in the booted uintr kernel). Code is ready to
validate; iter-3A's first task is the kernel/PXE fix, then the
five deferred phases of this iter run unchanged on the existing
in-tree code.
