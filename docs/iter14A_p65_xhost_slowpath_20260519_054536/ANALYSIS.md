# P6.5 analysis — xhost slowpath verification (counter-result)

**Date**: 2026-05-19
**Phase**: P6.5
**Hypothesis (user)**: P6 with NB=1M showed local_read ≈ xhost_read at
12.19 / 12.11 Mops/s. This is suspicious because the architecture has both
a local fast path (R2hit on local cache_pool) and a cross-host slow path
(R3 forward_read RTT ~10 µs). Likely cause: cache_pool absorbs all xhost
reads after warmup. With NB=1024 (2M keys / 1K buckets = heavy overflow),
cache_pool can't hold all keys → R2 miss → R3 forward_read fires on the
~50% xhost reads → quantify R3 cost.

## Cell layout

- T=64, KV=1024, cache=on, 3 reps each
- Scenarios: local_read, xhost_read × keyDists uniform, zipf
- NB values: 1024 (overflow) vs 1048576 (control, no overflow)
- 24 cells total, 0 FAIL

## Headline trans_agg_thpt (median across 3 reps)

| Scenario | keydist | NB=1024 | NB=1M | NB=1024/NB=1M |
|---|---|---:|---:|---:|
| local_read | uniform | 24.24M | 9.78M | **2.48×** |
| local_read | zipf | 23.29M | 12.39M | **1.88×** |
| xhost_read | uniform | 25.11M | 9.52M | **2.64×** |
| xhost_read | zipf | 22.79M | 12.37M | **1.84×** |

## Result: hypothesis falsified

**NB=1024 is FASTER than NB=1M, by 1.8-2.6×.** Opposite of prediction.

Across all 4 scenario×keydist combinations:
- NB=1024 throughput sits in the 22-25 Mops/s range
- NB=1M throughput sits in the 9.5-12.4 Mops/s range

And local vs xhost is still ~equal at both NB values (<2% difference).

## Why (revised mental model)

The MAX_OPS in P6.5 is 200,000 trans ops. With NB=1024 buckets, only
~8192 cache_pool slots exist (1024 × 8 probe slots per bucket). The
working set effectively collapses to ~8K hot keys regardless of the
original 2M trace key universe — only the first ~8K unique keys to be
accessed survive in cache_pool, and the remaining 192K ops repeat-access
those same keys.

This effectively forces 100% TLS-L1 hit rate (since 8K keys fit in the
1024-slot TLS — wait, that's still too small. Actually TLS = 1024 entries
per worker × 64 workers/host × 2 hosts = 131K entries total). So with
NB=1024 the system reduces to a "small working set" benchmark where
both TLS and cache_pool are warmed to the working set.

The NB=1M test, by contrast, has 2M keys × no overflow → cache_pool holds
everything → reads hit R2 successfully but at a wider TLS footprint
(working set = full 2M keys) so the per-thread TLS L1 doesn't fit. TLS
churn + cache_pool MESI ping-pong on the wider 17-cacheline-per-entry
hot set is what limits NB=1M to ~12 Mops/s.

## What this means

1. **P6 NB=1M is not bandwidth-limited.** It's structurally bound by the
   working-set-vs-TLS-size mismatch.
2. **Cross-host slow path (R3 RTT) is not visible in P6 nor P6.5.** Both
   regimes have local cache_pool fully populating from forward_read
   warmup. R3 fires once per cross-host key and then never again.
3. **For iter-15A R3-cost measurement, would need either**:
   - Continuous churn (read+evict cycle, e.g., insert phase mixed in)
   - Disable cache_pool entirely (FUSEE_CACHE=0 in a Protocol that
     respects it; Protocol A doesn't)
   - Probe-instrumented build to count R3 firings per op
4. **NB=1024 reaching 25 Mops/s** (xhost_read uniform) suggests the
   architecture CAN reach 20+ Mops/s when the working set fits the cache
   stack — confirming the TLS+cache_pool design works when sized
   appropriately.

## iter-15A backlog implications

- **TLS sizing study**: relationship between TLS entries, keys, working
  set, hit rate. P8 E2 (TLS counter dump) would quantify this.
- **R3 cost measurement** must use a different workload structure than
  static 2M-key load + 200K read trans.
