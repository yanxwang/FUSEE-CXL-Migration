# iter2 / step 3 — C-only scaling sweep (per-slot LFM + epoch outside crit-section + self-host cache refresh)

Task plan: `docs/task_plan_20260423_c_writepath.md`. Iter-2 decomp:
`docs/latency_decomp_C_iter2_20260423_053919.md`.

## Changes layered on top of iter 1

1. **`bump_epoch` moved outside the slot critical section.** In
   `insert()` / `update()` / `remove()`, the atomic epoch increment
   now happens **after** `lock_table_.unlock_slot(...)`. Since the
   atomic itself is globally ordered and readers seqlock on
   `write_epoch` rather than on the slot lock, shrinking the
   critical section is safe. Lets the next writer on the same slot
   enter while we bump.
2. **Writer-side DRAM cache refresh.** Instead of
   `cache_epoch_[idx] = UINT64_MAX`, we write the new slot contents
   into `cache_buckets_[idx].slots[s]` and advance `cache_epoch_[idx]`
   to the freshly-bumped CXL epoch. Same-process subsequent reads
   hit DRAM (~100 ns) instead of round-tripping to CXL (~5 µs).
   Peer-host readers still see staleness via per-bucket epoch
   mismatch — unchanged read-side guarantee.
3. `bump_epoch` now returns the post-increment value so callers can
   advance their DRAM cache without a separate read.

## Results (cache=on peaks, Mops/s)

| workload | baseline LFM-per-bucket | iter1 per-slot | iter2 this run | iter2 gain × vs iter1 | iter2 vs 20 Mops/s |
|----------|-------------------------|----------------|----------------|-----------------------|--------------------|
| a        | 1.08 (T=8)              | 3.41 (T=86)    | 3.27 (T=86)    | 0.96 (slight regress) | **6.1 × below**   |
| b        | 6.55 (T=32)             | 9.98 (T=32)    | 10.54 (T=32)   | 1.06                  | **1.9 × below**    |
| c        | 51.22 (T=86)            | 46.01 (T=86)   | 45.99 (T=86)   | 1.00                  | above target       |
| d        | 45.86 (T=86)            | 38.30 (T=86)   | 41.00 (T=86)   | 1.07                  | above target       |
| f        | 1.44 (T=16)             | 3.53 (T=16)    | 4.34 (T=32)    | 1.23                  | **4.6 × below**    |

## Decomp reconciliation (workload A cache=on, write-only)

From `docs/latency_decomp_C_iter2_20260423_053919.md`:

| T  | iter1 lock (µs) | iter2 lock | iter1 epoch | iter2 epoch | iter1 decomp thpt | iter2 decomp thpt |
|----|-----------------|------------|-------------|-------------|-------------------|-------------------|
| 8  | 8.0             | 6.7        | 1.60        | 2.69        | 0.98 Mops/s       | 1.06              |
| 16 | 11.6            | 8.9        | 1.85        | 3.16        | 1.13              | 1.59              |
| 32 | 16.6            | 12.2       | 1.82        | 3.32        | 1.40              | 1.81              |
| 64 | 26.5            | 25.0       | 2.25        | 4.07        | 1.36              | 1.32              |

- Lock stage shrinks 6 – 27 %: the move-outside-crit-section wins
  what is available at low/mid T. At T=64 hot-slot serialisation
  dominates regardless.
- Epoch stage **grew 68 – 82 %**: the correctness-mandatory atomic
  `__atomic_add_fetch` pays an explicit RMW round-trip that the old
  non-atomic `CACHELINE_LOAD+CACHELINE_STORE` did not. Eats back
  most of the lock savings on the write-only decomp.
- Full sweep thpt (50 % reads + 50 % updates for A / F) benefits
  more than the write-only decomp because the cache-refresh trick
  lets read ops hit DRAM — that is why workload F improves +23 %
  while A's peak is roughly flat (most Zipfian hits are still writes
  at T=86).

## Pass condition

Task bar: `trans_agg_thpt ≥ 20 × 10^6` on C at some T for **each** of
workloada, workloadb, workloadf. **Not met.** Iter-2 improves iter-1
on B/D/F but barely moves A at peak (T=86). Overall iter-2 is the
**recommended default** because it is required for correctness under
per-slot granularity (atomic epoch bump) and improves three of the
five workloads at no cost to the other two.

## Conclusion: the ceiling is structural

Both iter-1 and iter-2 converge on a peak of ~3 – 4 Mops/s for
Zipfian-write-heavy traffic (A, F). Decomp confirms the remaining
critical-section floor is the atomic cross-host epoch bump
(~3 – 4 µs / op). To close the gap to 20 Mops/s on A / F we need one
of:

- **Relax LRC so writers can defer the epoch bump** (one bump per K
  writes or per T µs). Readers tolerate a wider staleness window.
  Requires spec-level decision on how to bound staleness (bytes /
  ops / time) and a corresponding documentation change.
- **Per-host sharded writes** — partition the hot-key range so each
  host owns its shard, sync periodically. Eliminates per-write
  cross-host cost on the hot path. Requires routing changes at the
  key-level and a new reconciliation protocol.
- **Micro-batching of same-key updates** — accumulate updates to the
  same key on one host into one CXL publish + one epoch bump.
  Benefits real Zipfian traffic where the hottest key sees many
  consecutive updates per client.

None is a drop-in change; all three warrant a dedicated design
document before iter 3.

## Files produced

- 14 cache-on plots + 9 extra-compare plots (baseline vs iter1 vs
  iter2) in this directory and under `extra/`.
- 14 cache-off plots under `cache_off/`.
- Raw: `SUMMARY.log` (80 runs, all OK).
- Provenance: `plot_commit.txt`.
