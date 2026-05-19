# F2 RAP — LRU sampling (cache_pool_lookup write-on-read)

**Phase**: P4.3
**LOC**: 3
**Discovered**: P4 path_decomp R2hit = 0.23 µs p50 (7.7× spec expected 0.03 µs)

## STATE

[cache_pool.cc:90-93](../src/cxl_cache_pool.cc#L90-L93):

```cpp
if (value_size) *value_size = sz;
// LRU touch (relaxed RMW).
uint64_t ge = pool->global_epoch.load(std::memory_order_relaxed);
e->lru_epoch.store(ge, std::memory_order_relaxed);
return true;
```

`e->lru_epoch.store` on **every** hit → cross-core write of cacheline 0
(where lru_epoch lives). Under multi-reader Zipf, this turns "pure
read" into "every read writes" → cacheline 0 ping-pongs constantly.

## ATTACK VECTORS

1. **PERF**: every hit-path lookup pays MESI invalidate cost on cacheline 0
2. **CORRECTNESS**: LRU is approximate by spec; sampling preserves semantics
3. **GENERALITY**: applies on all hot Zipf workloads
4. **COMPLEXITY**: 3 lines, single conditional
5. **PRIOR ART**: standard "sampled LRU" approach (Memcached, RocksDB)
6. **FEASIBILITY**: trivial, no §I9 interaction

## ABLATION

Sample only 1/64 hits via `__rdtsc() & 0x3F == 0`.

## VERDICT

Tiny change, clear cause, verified pattern. Implement F2.

## DECISION

Implement under `FUSEE_LRU_SAMPLE=1` build flag.
