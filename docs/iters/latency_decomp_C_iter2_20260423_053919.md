# C write-path latency decomposition — iteration 2 (2026-04-23)

Fresh decomp after iter-2 changes, for feeding the step-1→3 loop from
`docs/task_plan_20260423_c_writepath.md`. Previous iter
(`docs/latency_decomp_C_iter1_20260423_050303.md`): per-slot LFM only.
This iter keeps the per-slot LFM base and layers two refinements:

1. **`bump_epoch` moved outside the slot critical section.** Atomic
   `__atomic_add_fetch` + `clflushopt` + `sfence` is globally ordered;
   readers seqlock on the bucket's `write_epoch`, not on the slot
   lock. Releasing the slot lock before the bump lets the next writer
   on the same slot enter immediately.
2. **Writer-side self-host DRAM cache refresh.** Instead of setting
   `cache_epoch_[idx] = UINT64_MAX` on UPDATE/DELETE, we update
   `cache_buckets_[idx].slots[s]` with the new contents and advance
   `cache_epoch_[idx]` to the freshly-bumped CXL epoch. Same-process
   readers hit DRAM on the next read of this bucket; peer-host
   readers still invalidate through the epoch mismatch.

## Decomp data (workload A, cache=on, 2 hosts)

| T  | lock avg/p50/p99 (µs)  | scan | publish | epoch | unlock | total avg/p50/p99 (µs) | write thpt (Mops/s) |
|----|------------------------|------|---------|-------|--------|------------------------|---------------------|
| 8  | 6.7 / 6.3 / 15.9       | 0.84 | 0.014   | 2.69  | 0.016  | 10.3 / 9.8 / 19.8      | 1.06               |
| 16 | 8.9 / 7.8 / 42.5       | 0.95 | 0.015   | 3.16  | 0.018  | 13.0 / 11.9 / 46.6     | 1.59               |
| 32 | 12.2 / 8.2 / 101.3     | 0.97 | 0.016   | 3.32  | 0.021  | 16.6 / 12.5 / 109.7    | 1.81               |
| 64 | 25.0 / 9.0 / 427.1     | 1.40 | 0.018   | 4.07  | 0.026  | 30.6 / 13.4 / 439.6    | 1.32               |

Comparing to iter-1 per-slot LFM (same cache-on workload A decomp):

| T  | iter1 lock_avg | iter2 lock_avg | iter1 epoch | iter2 epoch | iter1 thpt | iter2 thpt |
|----|----------------|----------------|-------------|-------------|------------|------------|
| 8  | 8.0            | **6.7** (-16 %) | 1.60        | 2.69 (+68 %) | 0.98       | **1.06** (+8 %) |
| 16 | 11.6           | **8.9** (-23 %) | 1.85        | 3.16 (+71 %) | 1.13       | **1.59** (+41 %) |
| 32 | 16.6           | **12.2** (-27 %)| 1.82        | 3.32 (+82 %) | 1.40       | **1.81** (+29 %) |
| 64 | 26.5           | 25.0 (-6 %)    | 2.25        | 4.07 (+81 %) | 1.36       | 1.32 (-3 %)   |

Observations:

- Lock stage shrinks by 6-27 % — the expected win from releasing the
  slot lock before the bump. Most visible at T=16-32 where the lock
  queue length matters.
- Epoch stage **grows** 68-82 %. That is the bill for atomic
  `__atomic_add_fetch`: every acquire on the write_epoch cacheline
  pays an explicit RMW roundtrip instead of a naive
  `CACHELINE_LOAD+CACHELINE_STORE`. The old non-atomic pattern could
  lose increments under per-slot granularity — fixing that is a
  correctness must-have (see
  `docs/g34_scaling_ycsb_C_only_20260423_051200/iteration_note.md`)
  — so the higher epoch cost is not optional.
- Net decomp-side throughput: +8 to +41 % across T=8…32. Flat at T=64
  because the hot-slot serialisation is still the dominant term.

The cache-refresh part of iter-2 does not show in the decomp runner
because decomp only exercises writes (same-client reads in that
harness are uninstrumented). The full YCSB sweep is what exposes its
win on 50 % reads.

## Implications

- The remaining gap vs the 20 Mops/s bar on workload A cannot be closed
  by shortening the write critical section alone: the atomic epoch
  bump itself is a ~3 µs cross-host operation that every writer pays.
  Further wins need either amortising the bump (one-bump-per-N-writes
  with readers tolerating the staleness, i.e. loosening LRC guarantees)
  or eliminating cross-host coherence for hot-key state (per-host
  shards with periodic sync).
