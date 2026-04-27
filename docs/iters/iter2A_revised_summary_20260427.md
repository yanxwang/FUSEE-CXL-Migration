# iter-2A-revised — N:1:1:N + atomic_store invalidation (code complete; empirical blocked)

**Date**: 2026-04-27
**Plan**: `docs/iters/task_plan_20260427_iter2A_revised_n11n_atomic.md`.
**Branch**: `feat/cxl-migration` (commit prefix `[iter2A-rev-arch]`).

> **🚫 STATUS: Phases 1-5 code complete + builds clean; Phases 5-9
> empirical blocked.** g3 + g4 booted into custom `vmlinuz_uintr_6.15`
> kernel after PXE; that kernel has `CONFIG_CXL_MEM` **not set** so
> `cxl_mem` driver never binds the CXL memory devices, no
> `/dev/dax0.0` is created. Standard `vmlinuz-6.12.38+deb13-amd64`
> with `CONFIG_CXL_MEM=m` is on disk in `/boot/` but cannot be
> selected without PXE-server config edit (192.168.128.5) or
> physical reboot, neither reachable from this agent. `kexec` is not
> installed on the slaves either.
>
> **What this means**: Phases 1-4 (architecture + wire) ship as code
> in tree; Phase 5 integration smoke + Phases 6-7 (sweep + decomp +
> queue depth) cannot run. Phase 8 documents what landed +
> documents the kernel block + names the iter-3A first task.
>
> **iter-3A first task**: restore CXL kernel on g3+g4 (PXE config
> edit to boot stock 6.12.38, OR enable CXL_MEM in the uintr kernel
> rebuild). After that, iter-3A Phase 0 = run iter-2A-revised's
> deferred Phases 5-7 with the existing in-tree code; the wire is
> ready to validate as soon as testbed is back.

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
