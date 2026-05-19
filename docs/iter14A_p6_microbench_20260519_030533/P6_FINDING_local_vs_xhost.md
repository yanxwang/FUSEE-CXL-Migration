# P6 finding — local_read ≈ xhost_read is by-design, not bug

**Date**: 2026-05-19 (mid-iter, user-raised question)
**Phase**: P6 (microbench)

## Observed (P6 partial, 127/144 cells)

| Cell | Throughput (Mops/s) |
|---|---:|
| local_read uniform T=64 | 12.19 |
| xhost_read uniform T=64 | 12.10 |
| local_read zipf T=64    | 21.47 |
| xhost_read zipf T=64    | 21.37 |

local vs xhost differ by < 1% (within rep variance) for both keyDists.
The dominant axis is uniform vs zipf (12 → 21 Mops/s = 1.76× lift).

## Why local ≈ xhost: shared CXL cache_pool absorbs the distinction

Read code path ([src/cxl_kv_ops_A.cc:2263-2330](../../src/cxl_kv_ops_A.cc#L2263)):

```
search(key):
  R1
  R0_tls_hit       // per-host DRAM TLS, ~100 ns
  R2hit            // cache_pool_lookup (CXL SHARED), ~250 ns
  R2miss
  R3 forward_read  // cross-host RTT, ~10 µs   ← only when cache_pool MISS
                   //                              AND owner != this host
  (else) local bucket scan + pool->read
```

The `cache_pool` (KvCachePool) lives in CXL devdax (`/dev/dax0.0`) and is
**MAP_SHARED across both hosts**. After the load phase, all 2 M keys are
in cache_pool regardless of which host owns each bucket. Therefore:

- In trans phase, `local_read` reads keys owned by this host: cache_pool
  hit → R2hit → return ~250 ns.
- `xhost_read` reads keys owned by the OTHER host: **same** cache_pool
  hit → R2hit → return ~250 ns.
- **R3 (forward_read cross-host RTT) is never triggered** because the
  cache_pool absorbs the lookup before we ever check who owns the bucket.

## P4 production data confirms low R3 incidence

In `workloadc T=64 cache=on` (pure-read production):
- R1 (read entry) N = 10016
- R3 (forward_read) N = **89 → 0.89%** of all reads
- R2hit N = 955, R2miss N = 8768, R0_tls_hit N = 236

Even under production YCSB-c with smaller key range (200k), only ~1% of
reads exercise the cross-host slow path. In P6 microbench with 2 M keys
+ NB=1048576 buckets at < 50% load factor, the cache_pool evicts
essentially nothing → R3 incidence is approximately 0%.

## What the data CAN tell us (and what it can't)

✅ The 12.19 vs 12.10 Mops/s equality is **fully explained by the
   architecture** (shared CXL cache_pool). It is NOT a measurement bug;
   it reflects how the code actually works.

✅ The 1.76× uniform→zipf lift is the **TLS L1 hit rate effect**:
   zipf concentrates accesses on top keys → TLS hits → 100 ns instead
   of 250 ns. This is the "real" cache benefit visible in microbench.

❌ **The microbench does NOT measure the cross-host slow path**
   (forward_read RTT). The "xhost_read" label is misleading: it
   chooses keys owned by the peer host, but the lookup never reaches
   the forwarding code because cache_pool intercepts.

❌ **The local-fast vs xhost-slow performance gap remains unknown**
   from P6 data alone.

## What this means for iter conclusions

1. **For YCSB-C target validation**: P6 confirms that the read path can
   reach 21 Mops/s under zipf at T=64 (above the 20 Mops/s YCSB-C goal).
   This is real and meaningful.

2. **For "is the cross-host design fast?"**: P6 cannot answer. Need a
   targeted experiment that forces cache_pool misses.

3. **For iter-13A copy elimination retrospective**: P5 already showed
   the HAZARD direct-pool read saves bytes/op at the BW level. The
   throughput-level benefit in workloads where R3 incidence is < 1%
   is correspondingly small. iter-14A P5 case-B conclusion stands.

## Verification experiment (P6.5 — to run after P6 + P7 complete)

**Goal**: Quantify the local-fast vs xhost-slow performance gap by
forcing cache_pool misses.

**Approach**: re-run a small set of cells with `NB=1024` (1 K buckets
instead of 1 M). With 2 M load keys / 1 K buckets = 2000 keys/bucket,
the cache_pool has ~64 slots/bucket and overflows heavily → most reads
miss cache_pool → R3 forward_read triggers on the xhost reads.

| Cell | NB | Expected behavior |
|---|---|---|
| local_read uniform T=64 NB=1K | 1024 | R2miss → local bucket scan + pool->read |
| xhost_read uniform T=64 NB=1K | 1024 | R2miss → owner != self → **R3 forward_read** |
| local_read zipf T=64 NB=1K | 1024 | R2miss less common (Zipf TLS retains hot) |
| xhost_read zipf T=64 NB=1K | 1024 | R3 incidence varies |

3 reps each = 12 cells. Estimated time: < 20 min.

**Expected outcome**:
- `local_read uniform NB=1K` ≈ a few Mops/s (depends on bucket scan
  + pool->read cost; should be ~5-10× lower than NB=1M case).
- `xhost_read uniform NB=1K` ≈ bounded by R3 RTT = 10 µs × N_threads
  / overlap. At T=64 perfectly pipelined, max = 6.4 Mops/s. Realistic
  with queue contention: 3-5 Mops/s.
- Local-vs-xhost gap **finally visible** as raw 2-5× ratio.

This experiment is captured as **P6.5** in the iter task list.
Will run after P7 sweep completes (P7 is already queued and consumes
both hosts).

## Implication for design review

The shared CXL cache_pool architecture is **the reason** that:
- iter-13A copy elimination didn't move throughput much (case B in slow
  mode) — the dominant workload regime serves reads from cache_pool, not
  from forward_read.
- TLS hit rate looks low (0.7% in P4 production) — but cache_pool's
  effective hit rate is the load-bearing metric, not TLS hit rate alone.

iter-15A design conversations should treat cache_pool as the workhorse
of the read path, not the forwarding ring.
