# C write-path latency decomposition — iteration 1 (2026-04-23)

Task: `docs/task_plan_20260423_c_writepath.md` step 1. North-star
`docs/design_goals.md`: C must hit **≥ 20 Mops/s trans_agg_thpt** on
workloads A, B, F at some T. Baseline (pre-iter-1): C workload A peaks
at **0.56 Mops/s** T=32 cache-on, i.e. **36× below target**.

## Instrumentation

`src/cxl_kv_ops_C.cc` insert/update/remove carry five stage timestamps
(`__dt0..__dt5`) under `-DFUSEE_LATENCY_DECOMP=1`:

| name     | boundary                                   |
|----------|--------------------------------------------|
| lock     | `lock_table_.lock()` returns                |
| scan     | 7-slot bucket scan + dup/target check done |
| publish  | `publish_slot` / value-store+flush done    |
| epoch    | `bump_epoch` returns                       |
| unlock   | `lock_table_.unlock()` returns             |
| total    | end-to-end insert/update                   |

Samples land in a process-local `DecompProbe` (see
`src/cxl_latency_decomp_probe.h`) which the fork harness
(`tests/cxl_latency_decomp_C.cc`) summarises per worker and merges at
the primary into a single `DECOMP_C` line per (T, cache).

A dedicated library `fusee_cxl_decomp` carries the same sources as
`fusee_cxl` but with `FUSEE_LATENCY_DECOMP=1` so production binaries
(`cxl_ycsb_runner_C`) stay unfenced and pay zero cost.

## Runs

Workload A (Zipfian 50/50 READ/UPDATE), 2 hosts g3+g4, cache=on, T ∈
{8, 16, 32, 64}. Each client is a fork child (per-client rdtscp-style
`clock_gettime` histograms, merged by primary). 200k trans ops split
200k/2T per worker; ~50 % are UPDATEs → 100k instrumented writes.

### Iter 1 — baseline (LFM per-bucket, current `main` behaviour)

`docs/latency_decomp_C_iter1_baseline_20260423_045354` — git at
`feat/cxl-migration`.

| T  | lock µs (avg/p50/p99) | scan | publish | epoch | unlock | total (avg/p50/p99) | trans_agg Mops/s |
|----|-----------------------|------|---------|-------|--------|---------------------|------------------|
| 8  | 14.0 / 5.5 / 143.3    | 1.1  | 0.016   | 1.42  | 0.016  | 16.6 / 7.9 / 146.5  | 0.59             |
| 16 | 51.5 / 5.7 / 500.6    | 1.3  | 0.021   | 1.59  | 0.023  | 54.4 / 8.7 / 504.1  | 0.39             |
| 32 | 140.5 / 6.0 / 941.5   | 1.3  | 0.031   | 1.55  | 0.035  | 143.4 / 8.9 / 944.3 | 0.27             |
| 64 | **572.3** / 7.3 / **12 788.7** | 1.5 | 0.047 | 1.70  | 0.055  | 575.6 / 10.7 / **12 792.7** | 0.14           |

**Conclusion**: lock acquire dominates 84 – 99 % of end-to-end latency.
`scan` (~1 µs), `publish` (~15–50 ns), `epoch` (~1.5 µs), `unlock`
(~15–50 ns) are all flat and small. The hot-bucket LFM-lock tail
explodes super-linearly with T; at T=64 the p99 lock wait is **12.8 ms**.
That is consistent with the baseline's hypothesis (Zipfian hot-bucket
contention on one LFM entry).

Compare-to-target: peak 0.59 Mops/s at T=8 → **34× below 20 Mops/s**.

### Iter 1 / step 2.1 — ticket-lock per bucket

Build with `-DFUSEE_USE_TICKET_LOCK=ON`. Two issues surfaced:

1. The ticket_lock.c shipped at `$CXL_SHM_PROFILING_DIR/locks/` was
   missing the pre-fetch-add `clflushopt` needed for cross-host
   correctness (the atomic RMW reads a host-local stale line before
   committing, so both hosts can hand out ticket 0). **Fix landed as
   `src/ticket_lock_fusee_patched.c`**, wired in via CMakeLists in
   place of the upstream object — keeps the fix inside the FUSEE repo
   rather than touching the shared `cxl_shm_profiling/` tree.

2. Even with the correctness fix, ticket-lock on the hot Zipfian
   bucket creates a cross-host clflushopt storm. T=8 workload A did
   not finish within the harness's wall-clock budget at ≈ 60 s per
   run (baseline finishes the same cell in ≈ 0.17 s, ≳ 350× slower).
   Each acquire has two mandatory `clflushopt`s on the same cacheline
   that every peer host is also polling; the fair FIFO serialises
   every cross-host roundtrip.

**Decision (per plan):** keep the build flag available but disable for
the sweep. Move on to step 2.2.

### Iter 1 / step 2.2 — per-slot lock (LFM-per-slot)

Build with `-DFUSEE_PER_SLOT_LOCK=ON -DFUSEE_USE_TICKET_LOCK=OFF`.
`SlotLockTable` uses 7 × `bucket_mutex_t` per bucket (LFM by default,
ticket optional). Writer flow:

- UPDATE/DELETE: unlocked scan → lock the found slot → re-verify key
  under lock → publish → bump_epoch → unlock.
- INSERT: unlocked scan → pick an empty-slot candidate → lock it →
  under-lock re-scan for dup in the other 6 slots + re-check target
  slot still empty → publish or retry.

Correctness: `cxl_kv_ops_C_test` (cross-host insert/update/delete)
passes on both g3 and g4. 2-host consistency preserved — readers
still seqlock on per-bucket `write_epoch`, which is unchanged.

Memory: 7 × `sizeof(shm_mutex_t)` per bucket ≈ 260 KiB/bucket ×
65 536 buckets ≈ **17 GiB** region. Trivial on the 512 GiB
`/dev/dax0.0`; init (`shm_mutex_init` on every slot) adds ~1 s to the
primary client's attach.

`docs/latency_decomp_C_iter1_perslot_lfm_20260423_051048`:

| T  | lock (avg/p50/p99)      | scan | publish | epoch | unlock | total (avg/p50/p99)   | trans_agg Mops/s |
|----|-------------------------|------|---------|-------|--------|-----------------------|------------------|
| 8  | 8.0 / 7.6 / 24.5        | 0.8  | 0.014   | 1.60  | 0.016  | 10.5 / 10.0 / 26.7    | 0.98             |
| 16 | 11.6 / 8.8 / 66.7       | 1.0  | 0.015   | 1.85  | 0.017  | 14.4 / 11.7 / 69.6    | 1.13             |
| 32 | 16.6 / 8.6 / 214.7      | 0.9  | 0.015   | 1.82  | 0.017  | 19.4 / 11.3 / 217.8   | 1.40             |
| 64 | 26.5 / 9.8 / 440.0      | 1.4  | 0.017   | 2.25  | 0.020  | 30.2 / 12.7 / 443.0   | 1.36             |

Speedups vs baseline:

| T  | baseline thpt | per-slot thpt | gain × | lock p99 drop |
|----|---------------|---------------|--------|---------------|
| 8  | 0.59          | 0.98          | 1.66   | 143 → 24 µs   |
| 16 | 0.39          | 1.13          | 2.87   | 500 → 67 µs   |
| 32 | 0.27          | 1.40          | 5.24   | 941 → 215 µs  |
| 64 | 0.14          | 1.36          | 9.41   | 12 788 → 440 µs |

Per-slot LFM peaks at **1.40 Mops/s at T=32 cache-on**.

Compare-to-target: still **~14× below 20 Mops/s**.

## Why per-slot LFM does not close the full gap

Zipfian workload A's hot-key traffic does not scatter across slots
within one bucket — each key is pinned to the single slot where LOAD
placed it. So per-slot lock only helps the rare case of two different
hot keys colliding in the same bucket. The dominant contention path
is still "all 2×T writers queue behind the one slot holding the
hottest key." The decomp confirms it: lock_avg at T=64 is still
26.5 µs and p99 is 440 µs — better than baseline but the critical
section itself (`epoch` = 2.3 µs per op) forms a 28 × lower bound on
per-key throughput (≈ 435 k ops/s per hot slot).

Hitting 20 Mops/s on Zipfian UPDATE-heavy traffic therefore demands
attacking the per-write critical-section cost itself, not just the
lock fairness. Options for iter 2 (not pre-committed; decide from the
fresh decomp under step 3's data):

- **CAS-based lock-free UPDATE**: skip the lock entirely and CAS
  slot.value. `publish` + `epoch` compress to two CXL flushes; lock
  acquire disappears. Correctness: UPDATE semantics (replace with
  newer value) permits racing writers to linearise by CAS order.
- **Per-host write batching**: coalesce ≥ N UPDATEs to the same key
  into one CXL publish on the owning host, serviced by a local
  writer thread. Trades per-op latency for throughput.
- **Replicated hot-cache with eventual consistency**: staleness
  budget Δ, each host updates its local DRAM + periodically pushes
  to CXL. Requires workload agreement on Δ.

## Next

Step 3 (C-only scaling sweep restricted to protocol C, 80 runs
A/B/C/D/F × T × cache) is running now; the results land under
`docs/g34_scaling_ycsb_C_only_<ts>/`. Iter-2 decision will be made
from that data, not from this report alone.
