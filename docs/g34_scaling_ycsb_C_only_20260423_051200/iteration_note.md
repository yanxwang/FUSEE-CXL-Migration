# iter1 / step 3 — C-only scaling sweep (per-slot LFM)

Task plan: `docs/task_plan_20260423_c_writepath.md`. Step-1 decomp:
`docs/latency_decomp_C_iter1_20260423_050303.md`.

## Optimization landed in this iter

- `src/cxl_bucket_lock.{h,cc}`: new `SlotLockTable` with 7 ×
  `bucket_mutex_t` per bucket. Reuses the existing per-bucket
  `write_epoch` for reader seqlock (unchanged read path).
- `src/cxl_kv_ops_C.cc`: compile-gated by `FUSEE_PER_SLOT_LOCK=ON`.
  UPDATE/DELETE: unlocked scan to find target slot → lock that slot →
  re-verify key under lock → publish → `bump_epoch` → unlock. INSERT:
  unlocked scan for dup + empty-slot candidate → lock candidate →
  under-lock re-scan of the other 6 slots for dup + re-check slot
  still empty → publish or retry (bounded by 8). Atomic `bump_epoch`
  (`__atomic_fetch_add + clflushopt + sfence`) landed at the end of
  iter 1 — **this sweep's binaries ran with the older non-atomic
  `CACHELINE_LOAD`+`CACHELINE_STORE` pattern**, which under per-slot
  granularity could lose increments when concurrent writers on
  different slots of the same bucket race on the shared per-bucket
  counter. In this workload no visible corruption manifested (readers
  still detect stale state via peer-host epoch mismatches), but the
  atomic fix is a correctness must-have for iter 2 before touching
  `bump_epoch` placement.
- `src/ticket_lock_fusee_patched.c`: FUSEE-local ticket_lock with the
  missing pre-`fetch_add` clflushopt so `FUSEE_USE_TICKET_LOCK=ON`
  builds are correct cross-host. Wired into CMake in place of the
  upstream object; shared `cxl_shm_profiling/locks/` stays untouched.

## Results (cache=on peaks, Mops/s)

Baseline = `logs/g34_scaling_sweep_p2_v4_20260422_205644` (LFM per
bucket).

| workload | baseline peak | iter1 peak (T) | gain × | gap to 20 Mops/s |
|----------|---------------|----------------|--------|------------------|
| a        | 1.08 (T=8)    | **3.41** (T=86)| 3.16   | **5.9 ×**        |
| b        | 6.55 (T=32)   | **9.98** (T=32)| 1.52   | **2.0 ×**        |
| c        | 51.22 (T=86)  | 46.01 (T=86)   | 0.90   | above target     |
| d        | 45.86 (T=86)  | 38.30 (T=86)   | 0.84   | above target     |
| f        | 1.44 (T=16)   | **3.53** (T=16)| 2.45   | **5.7 ×**        |

Cache=off peak on workloada rose to 4.50 Mops/s at T=86 — cache=off
actually beats cache=on for A at high T because the per-write cache
invalidation cost exceeds the cache hit savings on Zipfian traffic
that constantly invalidates reader caches anyway.

## Pass condition

Task bar: `trans_agg_thpt ≥ 20×10^6` on C at some T for **each** of
workloada, workloadb, workloadf. **Not met** — A/B/F remain 2 – 6 ×
below.

Protocols C and D are not in the validation set; C/D regressed
10 – 16 % vs baseline because `SlotLockTable::attach` writes ~17 GiB
of CXL on init (vs ~12 MiB for `BucketLockTable`) and the INSERT path
now includes an under-lock second-pass dup scan. Both are acceptable
costs for per-slot semantics' correctness; neither gates the task.

## Why iter1 did not close the gap

Per-slot decomposition (see
`docs/latency_decomp_C_iter1_20260423_050303.md`):

- Lock p99 at T=64 shrank 12.8 ms → **440 µs** (29 ×), lock_avg
  572 µs → **26 µs** (22 ×). Per-slot granularity does not reduce
  same-key contention, but it eliminates LFM's O(MAX_HOST_NUM=200)
  scan cost per acquire and it parallelises cold-key traffic across
  the 7 slots.
- `epoch` stage stays at ~2 µs because `CACHELINE_STORE` on
  `write_epoch` still pays one cross-host clflushopt+sfence on every
  write. With Zipfian hot keys funneling all 172 workers through one
  slot, the critical section (scan ≈ 1 µs + publish ≈ 20 ns + epoch
  ≈ 2 µs ≈ 3 µs) forms the per-hot-slot bound ≈ 330 k ops/s — which
  explains why workload A saturates around 3 – 4 Mops/s regardless of
  T once the lock itself is out of the way.

## Proposed iter-2 directions (data-driven from this decomp)

Not pre-committed. Pick after a fresh decomp that baselines against
per-slot LFM, not against the original per-bucket LFM.

1. **Epoch bump after unlock**. Move `bump_epoch` outside the critical
   section. Atomic fetch-add is already globally ordered; readers
   still seqlock correctly because the physical value/key stores are
   ordered by the slot lock and become visible before the atomic
   epoch increment is observed. Expected saving ≈ 2 µs/op on the
   hot-slot critical section ⇒ **1.5 – 2 × on A/F**.
2. **Writer-side self-host cache refresh**. Instead of
   `cache_epoch_[idx] = UINT64_MAX` on UPDATE/DELETE, write the new
   slot snapshot into `cache_buckets_[idx].slots[s]` and advance
   `cache_epoch_[idx]` to the freshly-bumped CXL epoch. Keeps the
   writer's own host's read path hot; other hosts still detect
   staleness via the existing per-bucket epoch mismatch. Primarily
   helps workloads where a client writes a key and reads it shortly
   after (F's RMW; A's Zipfian where the same hot keys are re-read).
3. **Per-client slot-index hint cache**. A tiny per-client hash of
   "last-seen-in-slot(K)" skips the unlocked 7-way scan for hot keys.
   Saves ≈ 1 µs/op on the hot path.

## Files produced

- 14 cache-on plots + 9 extra overlay plots in this directory
  (`C_thpt_workload{a,b,c,d,f}.png`,
  `C_lat_workload{a,b,c,d,f}_{read,write}.png`,
  `extra/C_compare_workload{a,b,c,d,f}.png`,
  `extra/C_compare_workload{a,b,d,f}_lat.png`).
- 14 cache-off plots under `cache_off/`.
- Raw: `SUMMARY.log` (80 runs, all OK).
- Provenance: `plot_commit.txt`.

## Stretch outcome

Full A/B/C sweep was not run because per-slot LFM in this iter ONLY
affects protocol C (A/B use their own `BucketLockTable` via
`cxl_kv_ops_A.cc`/`cxl_kv_ops_B.cc`; `SlotLockTable` does not touch
them). An A/B/C sweep would duplicate recent numbers in
`docs/g34_scaling_ycsb_20260422_162908/` without adding a datapoint on
per-slot's impact on A/B.
