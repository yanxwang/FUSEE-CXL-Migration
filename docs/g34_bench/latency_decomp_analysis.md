# Write-path latency decomposition on g3 (PCIe-switched CXL)

**Instrument**: `tests/cxl_latency_decomp.cc` — measures each primitive
of Option C's write path in isolation, against `/dev/dax0.0` on g3 with
1/2/4 forked contenders on the same bucket (worst case).

**Raw log**: `docs/g34_bench/latency_decomp_g3.log` (10 000 iterations
per N), plot: `docs/g34_bench/latency_decomp_g3.png`.

## Per-primitive averages (ns)

| Primitive                  | N=1    | N=2    | N=4    |
|----------------------------|-------:|-------:|-------:|
| P1 `BucketLockTable::lock` | 4 223  | 16 959 | 26 868 |
| P3 flush + full_fence (7 slots) | 431 | 354 | 350 |
| P4 store value + flush + store_fence | 20 | 21 | 22 |
| P5 bump `write_epoch` + flush + fence | 2 069 | 2 019 | 2 021 |
| P6 unlock                  | 23 | 22 | 24 |
| P7 optimistic search       | 2 504 | 2 458 | 2 658 |

## Total write-path latency (sum P1+P3+P4+P5+P6)

| N | sum (μs) | implied kops/s/host |
|---|---------:|---------------------:|
| 1 |  6.8     | 147 |
| 2 | 19.4     |  51 |
| 4 | 29.3     |  34 |

## Cross-check against observed end-to-end

`cxl_kv_bench_mp_C /dev/dax0.0 4 500 1.0 8192` on g3 (N=4, pure writes):
agg 403 k ops/s → ~100 k ops/s per host → ~10 μs per op. The decomp
predicts ~29 μs per op if *every* op hits the same bucket; in the real
bench, ops spread across 8 192 buckets so contention is roughly
(bucket_count / num_hosts) × lower, hence faster.

## Where to optimize

Reading the stacked bars:

1. **`lock_acquire` is 4 μs even at N=1**, and scales to 27 μs at N=4.
   This is the single biggest phase — everything else combined is <2.4 μs.
   The 4 μs floor is LFM's per-acquire cost (two cacheline stores + one
   cacheline load + fences) over the PCIe-switched fabric.

   - **LFM → ticket lock or MCS queue lock**: LFM's optimism costs nothing
     when uncontended but gives up ordering at N≥3, which is the source of
     the p99 tail at N=4 (83 μs p99 vs 20 μs p50). A ticket lock trades
     uncontended best case (5 μs vs 4 μs) for a bounded worst case.
   - **Bucket sharding**: if we can make each host mostly touch its own
     bucket range (e.g., by partitioning the key space), lock contention
     collapses to N=1 and we recover the 4 μs floor.

2. **`epoch_bump` is 2 μs** — one cacheline store + flush + fence on a
   different cacheline than the slot. At N=1 it's 31 % of total.

   - Pack `write_epoch` into the bucket cacheline itself (currently lives
     in the lock entry, a separate cacheline). Saves one fence round-trip.
     **This is the Option A packed-entry optimization** referenced in
     `docs/option_a_side_track.md` — worth retrying now that we have a
     clean bench.

3. **`flush_plus_fence` is 0.4 μs** — the cost of re-reading 7 slots'
   keys after getting the lock. Minor, but:

   - Shrink slots per bucket from 7 to 4 (RACE-style insert-only
     collisions are rare at realistic load factors). Saves 3 flush_lines
     per acquire → ~100 ns.

4. **`store_plus_flush` is 20 ns, `unlock` is 23 ns** — irrelevant.

## Read-path estimate

Search primitive (P7) = 2.5 μs for a hit at any contention level.
Because C's search doesn't take the lock, contention doesn't add cost.
This matches the MP bench observation that reads are essentially
protocol-independent (all three tied at ~1.2 M ops/s in 4-host read-only
YCSB cache-off).

## Next steps (plan intent)

- Task 2.1: retry packed-entry (put `write_epoch` in the bucket cacheline).
  Will collapse P5 into P4's fence → +30 % write throughput at N=1 if it
  works.
- Task 4: scale N via *threads* not processes (shared address space
  eliminates some of the LFM inter-proc cost).
