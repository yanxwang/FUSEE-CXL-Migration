# Multi-thread client scaling on g4 (PCIe-switched CXL, single process)

**Bench**: `tests/cxl_kv_bench_threads.cc` — one process, N pthreads,
each with its own LFM slot, shared `BucketLockTable` + bucket table
on `/dev/dax0.0`. Implements C semantics inline (lock + 7-slot probe
+ store + epoch bump + unlock). 5 000 ops per thread, 65 536 buckets.

**Raw log**: `docs/g34_bench/thread_scaling_g4.log` (15 runs = 3 wr × 5
N), plot: `docs/g34_bench/thread_scaling_g4.png`.

## Agg throughput (kops/s)

| threads | wr=0 (R) | wr=0.5 | wr=1 (W) |
|---------|---------:|-------:|---------:|
| 1       |  2 455   |    201 |     106  |
| 2       |  4 881   |    374 |     211  |
| 4       |  9 686   |    763 |     418  |
| 8       | 18 620   |  1 490 |     820  |
| 16      | 36 041   |  3 022 |   1 635  |

## Speedup vs 1 thread

| threads | wr=0   | wr=0.5 | wr=1   |
|---------|-------:|-------:|-------:|
|  2      | 1.99×  | 1.86×  | 1.99×  |
|  4      | 3.95×  | 3.79×  | 3.94×  |
|  8      | 7.58×  | 7.40×  | 7.74×  |
| 16      | 14.68× | 15.02× | 15.42× |

**Near-perfect linear scaling up to 16 threads**, across all workloads.
Scaling efficiency at 16T is 91–96 %.

## Why this scales so well

The write path is 8 μs per op (dominated by LFM lock acquire and epoch
bump). Each thread does ≤ 5 000 ops → wall ≤ 50 ms. With 65 536 buckets
and keys generated as `make_key(tid, i) = ((tid+1) << 40) | (i+1)`, the
hash spreads keys almost uniformly across buckets. Collision probability
for any single bucket is 5 000 × 16 / 65 536 ≈ 1.2 ops per bucket over
the whole bench. So lock contention is essentially zero and scaling is
bounded only by memory-side throughput.

At 16 threads × 820 k writes/s = 13 M writes/s aggregate. Each write
does 3 cacheline stores + flushes (value, epoch, unlock) = 39 M flushes/s
= 2.5 GB/s of store traffic into the memory server. PCIe × CXL
bandwidth on the switch fabric comfortably handles this.

## Latency stays flat

Mean write latency at every N: 8–9 μs. Mean read latency: 1.9–2.1 μs.
No queueing tail because no contention.

## What this tells us

- **Thread scaling is free on g4** up to 16T for random-key workloads.
- If a workload induces bucket hotspots (e.g. YCSB Zipfian), scaling
  will degrade proportionally to the hotspot concentration.
- This is the preferred client concurrency dimension for FUSEE-CXL:
  one process per machine, N threads per process. `num_threads` in
  the bench maps to what the original RDMA-FUSEE called "client threads"
  (minus the coroutine layer, which is not needed without RDMA latency
  to hide).

## Implications for task 5 (cross-host sweep)

For the 2-host cross-host sweep (g3 + g4), role-mode would give us
2 processes × N threads per process = 2N parallel clients. With N=8
we'd simulate 16 concurrent clients at real cross-host workload. That
requires extending the role-mode bench runner to also support threads,
which is a small lift — the thread code works, we just need to add a
FUSEE_NUM_THREADS env + per-thread host-id mapping.

Leaving that extension for task 5's own doc; this one ends here.
