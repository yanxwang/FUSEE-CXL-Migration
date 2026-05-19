# P6 finding — local_read ≈ xhost_read after warmup (CORRECTED)

**Date**: 2026-05-19 (mid-iter, user-raised question)
**Phase**: P6 (microbench)
**Revision history**:
- v1 (initial, **INCORRECT**): claimed cache_pool was shared CXL — wrong.
- v2 (this version, corrected): cache_pool is per-host DRAM; the equality
  comes from R3 warmup populating each host's local cache_pool, so
  steady-state reads R2hit locally regardless of original ownership.

## Observed (P6 partial)

| Cell | Throughput (Mops/s) |
|---|---:|
| local_read uniform T=64 | 12.19 |
| xhost_read uniform T=64 | 12.10 |
| local_read zipf T=64    | 21.47 |
| xhost_read zipf T=64    | 21.37 |

local vs xhost differ by < 1% (within rep variance).

## Cache_pool location (FACT CHECK)

[tests/protocol_a_ycsb.cc:273-275](../../tests/protocol_a_ycsb.cc#L273):

```cpp
void *cache_mem = mmap(nullptr, cache_pool_bytes(num_buckets),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
```

`MAP_ANONYMOUS` = no backing file = **DRAM only**.
`MAP_SHARED` = shared across the forked workers of THIS host process.
Each host (g3 / g4) runs its own protocol_a_ycsb process and mmaps its
own private cache_pool. **The two cache_pools are NOT shared between
hosts.**

## Why local_read ≈ xhost_read: R3 warmup + cache_pool re-population

### Insert path
[src/cxl_kv_ops_A.cc:450-457](../../src/cxl_kv_ops_A.cc#L450):
```cpp
uint32_t owner = owner_host(key);
if (owner != (uint32_t)host_id_) {
    return forward_write(owner, key, value, value_len, kOpKindInsert);
    // forward only; does NOT insert into local cache_pool
}
return execute_write_local(key, value, value_len, kOpKindInsert);
// inside execute_write_local: cache_pool_insert on this host
```

After load phase (both hosts each INSERT all 2M keys):
- h0's cache_pool has ~1M entries (the ~1M keys owned by h0)
- h1's cache_pool has ~1M entries (the ~1M keys owned by h1)
- Cross-ownership keys are **NOT** in the caller's local cache_pool.

### Search (read) path
[src/cxl_kv_ops_A.cc:2263-2331](../../src/cxl_kv_ops_A.cc#L2263):

```
R1                      — probe entry
R0_tls_hit              — TLS L1 (per-thread DRAM) → return
R2hit                   — cache_pool_lookup hit (per-host DRAM) → return
R2miss                  — cache_pool miss
owner == self ?
  no →  R3              — forward_read_direct (cross-host RTT ~10 µs)
                        → on success, cache_pool_insert into LOCAL
                        → subsequent reads of same key are R2hit
  yes → owner-self path — bucket scan + pool->read
                        → cache_pool_insert into LOCAL
                        → subsequent reads R2hit
```

**Critical line** at [src/cxl_kv_ops_A.cc:2317-2324](../../src/cxl_kv_ops_A.cc#L2317):
```cpp
if (vlen > 0) {
  cache_pool_insert(cache_, key, v, vlen);  // populate LOCAL cache_pool
  if (g_thread_tls) {
    tls_insert(...);  // populate LOCAL TLS too
  }
}
```

This is **R3-then-cache** behavior: first read pays R3 cost (~10 µs),
subsequent reads of the same key go through R2hit (~250 ns) on the local
cache_pool, never reaching R3 again.

### Steady-state vs warmup

For trans phase with 1 M ops on 1 M unique keys (uniform):
- Random sampling with replacement → ~63 % unique keys hit, ~37 % repeats
- First reach of each unique key: R3 (xhost_read scenario) or owner-self
  bucket scan (local_read scenario) — slow path
- Subsequent reach: R2hit either way
- After warmup, both scenarios converge to "mostly R2hit" → ~250 ns/op

At T=64 the slow-path cost gets amortized; the aggregate throughput
becomes bounded by worker-side per-op overhead (~5 µs/op observed,
matches 64 / 5 µs = 12.5 Mops/s). The slow vs fast path differential
is **NOT** the dominant cost in this regime.

## Why this matters

iter-13A's HAZARD direct-pool-read was supposed to eliminate the
"owner copies pool→staging" step on the slow path. P5 evidence showed
B/op down 28 % but throughput unchanged in slow mode. The reason is the
SAME structural fact surfaced here:
- The slow path (R3 cross-host forward_read) only fires on the FIRST
  miss for each key, then gets cached locally.
- At steady state, hot keys are served from local cache_pool or TLS.
- Optimizing the slow path saves bytes on warmup, but warmup is a tiny
  fraction of total reads.

## Partial evidence from P4

P4 workloada T=64 cache=on (Zipf mixed RW, h0 view):
| Stage | N | % of R1 |
|---|---:|---:|
| R1 (read entry) | 5002 | 100 % |
| R0_tls_hit | 36 | 0.7 % |
| R2hit | 660 | 13.2 % |
| R2miss | 4260 | 85.2 % |
| R3 (cross-host forward) | **47** | **0.9 %** |
| owner-self pool->read | ~4213 | ~84 % |

In production workloada Zipf, **R3 incidence is 0.9 %**. The vast
majority of reads either hit local cache_pool/TLS or hit the local
bucket scan path (owner-self). This is direct evidence that the
cache_pool warmup is fast and cross-host slow path is rare in steady
state.

**However**: P4 numbers are for workload-a (Zipf, mixed RW), not for
the P6 microbench's local_read vs xhost_read scenarios. To directly
verify the P6 scenarios' R3 incidence, **we need probe data on the P6
cells specifically**. This was not captured in the current P6 run
(P6 used FUSEE_PROBE=0 by default for throughput accuracy).

## Verification experiment (P6.5) — UPDATED design

Original P6.5 design: just throughput at NB=1024 vs NB=1M.
Updated P6.5 design: include FUSEE_PROBE=1 cells to capture STAGE
COUNTS directly, so we can answer:

1. local_read uniform NB=1M: how many R3 fire? (expected: ~63 % of
   unique reads = ~63 % of 63 % = ~40 % of all reads — or maybe lower
   if owner-self path is what gets hit)

   Actually wait — local_read uniform means h0 reads h0-OWNED keys. So
   owner == self ALWAYS. R3 should NEVER fire. The slow path is
   "owner-self bucket scan + pool->read", which is what happens on
   R2miss + owner == self.

2. xhost_read uniform NB=1M: how many R3 fire? (expected: ~63 % of all
   reads, since these are h1-owned keys not in h0's cache_pool until
   first read)

3. NB=1024 cells: cache_pool overflows → R2 misses dominate → R3 fires
   on every xhost read.

The probe-enabled P6.5 will produce a per-scenario stage count table
that directly answers the user's question with measured evidence.

## What was previously wrong + corrected

| v1 claim | v2 correction |
|---|---|
| "cache_pool is in CXL devdax, shared between hosts" | cache_pool is per-host DRAM (MAP_ANONYMOUS) |
| "After load, all 2M keys are in shared cache_pool → R2hit for both local and xhost" | Each host's cache_pool has only its OWN owned keys (~1M each). xhost_read first-touch triggers R3, then populates local cache_pool. |
| "R3 is NEVER triggered" | R3 IS triggered on first read of each xhost-owned key; warmup then makes subsequent reads R2hit. |
| "Microbench cannot measure slow path" | Microbench measures slow path during warmup; the equality after warmup reflects amortization, not absence. |

The fundamental conclusion that "we can't tell the slow vs fast path
gap from the P6 throughput data alone" still stands — but for a
different reason (warmup vs steady-state amortization, not shared
cache_pool absorption).
