# Task plan — variable KV value size (256 / 512 / 1024 B) for protocol C

**Author**: Claude  
**Date drafted**: 2026-04-24  
**Status**: DRAFT v2 — user review points partially decided; awaiting
final sign-off before Phase-1 implementation  
**Branch**: `feat/cxl-migration` (new commits on same branch, `[kv-varlen]` prefix)

## Summary of design decisions (as of 2026-04-24, pending final review)

| # | Question | User position | Claude recommendation | Status |
|---|----------|---------------|-----------------------|--------|
| 1 | Scope | C only, A/B untouched | same | ✅ decided |
| 2 | Single-path vs dual-path backward compat | lean dual | **dual** | ⏳ pending |
| 3 | Add value_size=8 pooled baseline sweep | asked "what's the point" | **add** (isolates layout cost from size cost — otherwise iter-3→iter-4 throughput delta conflates two variables) | ⏳ pending |
| 4 | Compile-time (3 binaries) vs runtime env_var (1 binary) value size | asked "what's the difference" | **runtime env var** (<5 % memcpy penalty lost under CXL µs latency; 3× less build/deploy; easier to add more sizes) | ⏳ pending |
| 5 | Lazy free of old blocks | can be spec-only for now | **document-only, empty stub** (real sweep peak = N_keys × value_size ≈ 100 MiB, well under 2 GiB pool, so no leak risk within scope) | ⏳ pending |

Section 8 below has the detailed rationale for each. Sections 3–7
below assume these recommendations are accepted — I will revise if
any is rejected.

---

## 1. Motivation

Current FUSEE-CXL uses **inline 16 B KV** (8 B key + 8 B value). All
iter-3 results (peaks: A 17.05, B 50.24, C 64.96, D 52.27, F 21.31 Mops/s)
are under this simplification. The original FUSEE design supports
variable-length KV via pointer-to-block layout, which matches realistic
workloads (YCSB default value = 1 KB).

The hypothesis for this task:

> **As value size grows, the dominant bottleneck shifts from
> `bump_epoch` latency (current) toward CXL write bandwidth.**
> Single-host CXL seq_write = 22 GB/s (measured). At value_size ≥ 1 KB,
> each UPDATE moves ≈ 1088 B (bucket write + value block write),
> capping throughput at ~20 Mops/s from BW alone. At 256 B the BW
> ceiling is 68 Mops/s, leaving latency optimization still relevant.

Purpose of the three sweeps: **find the crossover point where FUSEE-CXL
protocol C transitions from latency-bound to bandwidth-bound**, and
confirm the 20 Mops/s north-star bar is or is not achievable under
realistic value sizes.

## 2. Scope

### In-scope
- Add CXL-resident variable-length block pool
- Migrate slot layout from inline value → pointer-to-block
- Widen `insert/update/search` APIs to (void *, uint32_t len)
- **Keep dual path in the store class**: if `blockpool_ == nullptr`
  and value_len ≤ 8, take the existing inline-u64 fast path (zero
  perf change vs iter-3); otherwise route through pool. This
  preserves bit-identical behaviour for historical 16-B tests and
  keeps iter-3 number comparability.
- Extend micro-batching ring to carry `blk_off` instead of inline value
  when blockpool is active (no size change in ring entry)
- YCSB runner generates value buffer of `FUSEE_VALUE_SIZE` bytes
  (**runtime env var**, single binary)
- Run **four** scaling_ycsb_C_only sweeps at value_size ∈ **{8, 256,
  512, 1024}** (the 8-byte pooled run is the apples-to-apples
  baseline isolating layout cost from size cost)
  with the canonical `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_BATCH_MERGE_SAME_KEY=ON` config and 2 M ops per trans phase
- Emit 4 summary plots + 1 cross-size comparison plot

### Out-of-scope (explicit non-goals)
- Protocols A and B: keep inline-16B only. ABC parity for variable
  KV would double the work and is not needed for the crossover study.
- Crash recovery: OpLog not updated to log value bytes. The three
  sweeps use ephemeral block pools.
- Variable-length KEY (keys stay u64).
- Block pool free-list reclamation (bump-alloc only; 512 GiB devdax
  accommodates all three sweep workloads).
- Directory splits / fingerprints (Phase-3 subtable scope unchanged).

## 3. Phased breakdown

Each phase ends with a green checkpoint (compile + smoke + sanity run).
All commits carry prefix `[kv-varlen]`. Implementation order:

| # | Phase | Deliverable | Est | Verify |
|---|-------|-------------|----:|--------|
| 0 | Design review | This doc approved | — | user sign-off |
| 1 | Block pool primitive | `src/cxl_kv_blockpool.{h,cc}` builds; standalone unit test passes | 45 min | `tests/cxl_blockpool_test.cc` |
| 2 | Slot relayout + back-compat | `CxlKvSlot.blk_off` rename; inline wrapper works; all existing tests still pass | 30 min | `cxl_ycsb_runner_C` 16 B smoke test |
| 3 | Protocol C write path | `insert/update(void*, len)` route through block pool | 1.5 h | `cxl_kv_bench_C` passes with 256 B values |
| 4 | Protocol C read path + cache | `search(void*, cap, *len)` returns value; DRAM cache extended to cache value block | 1.0 h | Read-after-write consistency test |
| 5 | Batching ring field rename | `RingEntry.new_value` → `blk_off`; flusher reads blk_off and writes slot | 20 min | Phase-3 batching smoke test unchanged |
| 6 | YCSB runner + CMake | value buffer generation, region sizing, SUMMARY.log `value_size=` field, `-DFUSEE_VALUE_SIZE=N` compile option | 1 h | Single-host 100-op run @ each size |
| 7 | Two-host smoke sweep | 1 workload × 1 T at each size | 30 min | No deadlock, throughput ≥ 1 Mops/s |
| 8 | Four full sweeps | 4 × 80-run `scaling_ycsb_C_only` at value_size = 8 / 256 / 512 / 1024 (env var switches between them, single binary) | 4 × 30 min = 2 h | ok=80 fail=0 each |
| 9 | Analysis + plots | Per-sweep `iteration_note.md`, cross-size comparison plot (throughput + p50 vs value_size), update `docs/scaling_ycsb_runs_index.md` | 1 h | Final review |

**Total: ~8–9 h. No destructive changes to existing files that are
not in this list.**

## 4. File-by-file change list

### 4.1 NEW: `src/cxl_kv_blockpool.h` (~45 LoC)

```cpp
#ifndef FUSEE_CXL_KV_BLOCKPOOL_H_
#define FUSEE_CXL_KV_BLOCKPOOL_H_

#include <cstddef>
#include <cstdint>

namespace fusee {

// Per-host bump-allocated block pool on CXL. Two-host usage: each host
// attaches to its own segment (host_id picks the segment). Writers
// flush on publish so peer host's reads see coherent values.
class CxlKvBlockPool {
 public:
  // Byte footprint for N hosts, each getting num_blocks_per_host slots
  // of block_size bytes. Hosts' segments are cacheline-padded.
  static size_t bytes_for(uint32_t num_blocks_per_host,
                          uint32_t block_size, int num_hosts);

  int attach(void *base, size_t bytes,
             uint32_t num_blocks_per_host, uint32_t block_size,
             int host_id, int num_hosts, bool init_region);

  // Allocate a new block from this host's segment. Returns absolute
  // offset within the pool region. Returns UINT64_MAX on exhaustion.
  uint64_t alloc();

  // Write + clflushopt + sfence so peer host will observe the value
  // once it follows a blk_off pointer. `len` must be <= block_size.
  void write(uint64_t off, const void *data, uint32_t len);

  // Read, issuing clflushopt + mfence on reader side so we see the
  // latest peer-host write (FUSEE's CACHELINE_LOAD pattern).
  void read(uint64_t off, void *out, uint32_t len) const;

  // Push old offset onto this host's lazy free-list. Not reclaimed
  // within the sweep scope; kept for future GC hook.
  void free_lazy(uint64_t off);

  uint32_t block_size() const { return block_size_; }

 private:
  uint8_t *base_ = nullptr;
  uint32_t block_size_ = 0;
  uint32_t num_blocks_per_host_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  std::atomic<uint64_t> *my_bump_ = nullptr;  // points into CXL header
};

} // namespace fusee
#endif
```

### 4.2 NEW: `src/cxl_kv_blockpool.cc` (~75 LoC)

Straight-forward implementation:
- Layout: `[header (num_hosts × 64 B, each holds bump_cursor)] [host_0 segment] [host_1 segment]`
- `alloc` = `my_bump_->fetch_add(block_size_)` (no CAS needed — single-host allocator)
- `write` = `memcpy + clflushopt-per-cacheline + sfence`
- `read` = `clflushopt-per-cacheline + mfence + memcpy`

### 4.3 MODIFY: `src/cxl_hashtable.h` (2 lines)

```diff
 struct CxlKvSlot {
   uint64_t key;
-  uint64_t value;
+  uint64_t blk_off;  // byte offset into CxlKvBlockPool; 0 means empty slot
 };
```

Layout / size / padding unchanged. Bucket still 128 B = 2 cachelines.
`kEmptyKey = 0` semantics preserved.

### 4.4 MODIFY: `src/cxl_kv_ops_C.h` (~20 LoC)

Add new overloads + pool reference + back-compat wrappers:

```cpp
class CxlKvStoreC {
  // ... existing fields ...
  CxlKvBlockPool *blockpool_ = nullptr;

 public:
  // Attach a pool for variable-length values. Not required if only
  // used via the inline (u64) API.
  void set_blockpool(CxlKvBlockPool *bp) { blockpool_ = bp; }

  // New variable-length API.
  int insert(uint64_t key, const void *value, uint32_t value_len);
  int update(uint64_t key, const void *value, uint32_t value_len);
  int search(uint64_t key, void *out_buf, uint32_t out_cap,
             uint32_t *out_len) const;

  // Backward-compat wrappers (keep existing tests building unchanged).
  int insert(uint64_t key, uint64_t value) {
    return insert(key, &value, sizeof(value));
  }
  int update(uint64_t key, uint64_t value) {
    return update(key, &value, sizeof(value));
  }
  int search(uint64_t key, uint64_t *out) const {
    uint32_t got = 0;
    return search(key, out, sizeof(*out), &got);
  }
};
```

**Semantics (dual-path design)**:
- If `blockpool_ == nullptr`, the new variadic API errors out
  (-1); only the u64 wrappers are usable. Used by historical tests
  (`cxl_kv_bench*`, A/B protocols) unchanged.
- If `blockpool_ != nullptr`, the u64 wrappers still take the inline
  fast path (store `value` directly into `slot->blk_off` field; no
  pool allocation). This preserves the iter-3 16 B inline data-path
  in bit-identical form when `FUSEE_VALUE_SIZE=8` is used.
- If `blockpool_ != nullptr` AND `value_len > 8`, the variadic API
  allocates a pool block and stores `blk_off` in the slot.

This dual-path design isolates "layout overhead" (inline vs pooled)
from "size overhead" (8 B vs 1 KB) in the measurements. The
`FUSEE_VALUE_SIZE=8` apples-to-apples baseline sweep is what reveals
the pure layout cost — see §7.

### 4.5 MODIFY: `src/cxl_kv_ops_C.cc` (~150 LoC)

Sketch of UPDATE, the critical path. INSERT is analogous; DELETE is
unchanged except it calls `blockpool_->free_lazy(old_off)` after
publishing empty slot.

```cpp
int CxlKvStoreC::update(uint64_t key, const void *val, uint32_t len) {
  if (!blockpool_) return -1;
  if (len > blockpool_->block_size()) return -1;

  // Step 1: allocate + write value to CXL BEFORE taking the lock.
  uint64_t new_off = blockpool_->alloc();
  if (new_off == UINT64_MAX) return -1;
  blockpool_->write(new_off, val, len);

  // Step 2: existing C write path, with one substitution:
  //   slot->value = value     →     slot->blk_off = new_off
  // The LFM acquire / scan / publish / epoch / unlock flow is byte-
  // for-byte unchanged; only the payload being published differs.
  uint32_t bidx = bucket_idx(key);
  // ... lock_slot / scan / match / publish(new_off) / bump_epoch ...
  uint64_t old_off = matched_slot->blk_off;  // read before overwrite
  matched_slot->blk_off = new_off;
  // ... epoch + unlock ...

  // Step 3: queue old block for lazy free.
  if (old_off != 0) blockpool_->free_lazy(old_off);
  return 0;
}
```

For **SEARCH**:

```cpp
int CxlKvStoreC::search(uint64_t key, void *out, uint32_t cap,
                         uint32_t *len) const {
  // ... existing seqlock-style bucket scan, find matched slot ...
  uint64_t off = matched_slot.blk_off;  // (read from cached bucket)
  uint32_t n = std::min(cap, blockpool_->block_size());
  blockpool_->read(off, out, n);
  *len = n;
  return 0;
}
```

**Cache-on extension**. The existing `cache_buckets_` / `cache_epoch_`
only caches the 128 B bucket. For variable KV we add:

```cpp
mutable std::vector<uint64_t> cache_blocks_off_;   // cached blk_off
mutable std::vector<uint8_t>  cache_blocks_data_;  // cached value bytes
mutable std::vector<uint64_t> cache_blocks_epoch_; // for invalidation
```

Cache hit conditions: (a) bucket cache hit AND (b) cached blk_off
matches the slot's current blk_off. On mismatch, fetch from pool +
update cache. **This keeps cache-on meaningful for read workloads
under the new layout**; without it, cache-on would still only cache
the slot, and every read would pay the block read regardless.

### 4.6 MODIFY: `src/cxl_batch_ring.h/cc` (~10 LoC)

```diff
 struct RingEntry {
   uint16_t slot_idx;
   uint16_t flags;
   uint32_t seq;
-  uint64_t new_value;
+  uint64_t blk_off;  // same size / layout; semantics: offset into blockpool
 };
```

`MicroBatchRing::append` signature is unchanged (still takes a u64
payload). Flusher in `cxl_kv_ops_C.cc` substitutes
`slot->value = entry.new_value` → `slot->blk_off = entry.blk_off`.
**No change to ring size, K, dedup, or MPSC flag semantics.**

### 4.7 MODIFY: `tests/cxl_ycsb_runner.cc` (~60 LoC)

1. Read value size from **env var** (runtime, single binary):
   ```cpp
   uint32_t kValueSize = 8;
   if (const char *env = getenv("FUSEE_VALUE_SIZE")) {
     kValueSize = static_cast<uint32_t>(atoi(env));
     if (kValueSize == 0) kValueSize = 8;
   }
   ```

2. Pre-allocate a per-thread value buffer:
   ```cpp
   std::vector<uint8_t> v_buf(kValueSize);
   ```

3. Generate deterministic value from key for each op:
   ```cpp
   for (uint32_t i = 0; i < kValueSize; i++)
     v_buf[i] = static_cast<uint8_t>((o.key >> (8 * (i & 7))) ^ (i * 31));
   ```

4. Route ops through new API:
   ```cpp
   case OP_INSERT: rc = store.insert(o.key, v_buf.data(), kValueSize); break;
   case OP_UPDATE: rc = store.update(o.key, v_buf.data(), kValueSize); break;
   case OP_READ: {
     uint8_t out[kValueSize]; uint32_t got;
     rc = store.search(o.key, out, kValueSize, &got);
   } break;
   ```

5. Region sizing (primary client allocates; pool segment attached
   only when `kValueSize > 8` to keep 8-B-inline runs bit-identical):
   ```cpp
   bool use_pool = (kValueSize > 8);
   size_t pool_bytes = 0;
   if (use_pool) {
     pool_bytes = fusee::CxlKvBlockPool::bytes_for(
         /*num_blocks_per_host=*/est_ops * 2,
         kValueSize, num_hosts);
   }
   size_t total = kv_bytes + pool_bytes;
   ```

   Note: `kValueSize == 8` with pool attached is a valid
   configuration used by the apples-to-apples baseline sweep. In
   that case the pool is created but u64 wrapper keeps the inline
   fast path, so pool memory is reserved but unused — overhead is
   only the region-sizing cost (~2 GiB committed, not touched).
   For the pure-historical `kValueSize == 8` case (no pool), pass
   `nullptr` to `set_blockpool` — this matches the iter-3 inline-16-B
   runs exactly.

6. SUMMARY line extension (one field added, all existing parsers
   ignore unknown fields):
   ```
   YCSB opt=C cache=1 value_size=1024 num_hosts=2 threads=86 ...
   ```

### 4.8 MODIFY: `src/CMakeLists.txt` (~2 LoC)

```cmake
# cxl_kv_blockpool.cc is protocol-agnostic; link into the shared
# fusee_cxl object list so A/B/C all see the symbol (though only C
# links the store class that uses it).
list(APPEND source_fusee_cxl cxl_kv_blockpool.cc)
```

**No compile-time `FUSEE_VALUE_SIZE` macro** — value size is read
from env var at runtime (see §4.7). One binary supports all four
sweep configurations.

## 5. Data layout on CXL (updated)

```
[Header                4 KiB  ]
[BucketLockTable       ...    ] ← size unchanged
[CxlKvBucket[nb]       nb×128B] ← size unchanged
[BlockPool header      num_hosts × 64 B ]  ← new
[BlockPool host_0 seg  num_blocks × block_size ]  ← new
[BlockPool host_1 seg  num_blocks × block_size ]  ← new
```

For the three sweeps:

| value_size | bytes per op | 2 M-op pool / host | total pool (2 hosts) |
|-----------:|-------------:|-------------------:|---------------------:|
| 256 | 256 | 512 MiB | 1.0 GiB |
| 512 | 512 | 1.0 GiB | 2.0 GiB |
| 1024 | 1 024 | 2.0 GiB | 4.0 GiB |

All well within 512 GiB devdax.

## 6. Test matrix

Each sweep reuses the canonical script (`scripts/run_g34_scaling_ycsb_C_only.sh`)
with one added env var — **single binary, four sweeps**:

```bash
FUSEE_VALUE_SIZE=8    ./scripts/run_g34_scaling_ycsb_C_only.sh   # apples-to-apples baseline
FUSEE_VALUE_SIZE=256  ./scripts/run_g34_scaling_ycsb_C_only.sh
FUSEE_VALUE_SIZE=512  ./scripts/run_g34_scaling_ycsb_C_only.sh
FUSEE_VALUE_SIZE=1024 ./scripts/run_g34_scaling_ycsb_C_only.sh
```

Each emits to `docs/g34_scaling_ycsb_C_only_<ts>_kv<N>/` with the
same layout as existing sweeps (SUMMARY.log, 14 plots, extra/
cross-iter overlays, iteration_note.md).

**Baseline rationale**: the `kv8` sweep uses the **new pointer
layout** (`blockpool_ != nullptr`, but u64 wrapper keeps inline
fast path). Comparing against iter-3 historical 16-B numbers
isolates whether the layout *refactor itself* introduced regressions
vs the size increase. If kv8 matches iter-3 within ±5 %, the
layout change is confirmed neutral and all kv-256/512/1024 deltas
are attributable to value-size effects alone.

**Pre-sweep correctness check** (runs before the three sweeps):

- Single-host smoke: `cxl_kv_bench_C --value-size 1024 --ops 10000` at
  each size, confirm no memory corruption + read-back matches write.
- Two-host smoke: load 10 k keys on host 0, search all on host 1,
  confirm 100 % hit rate and value bytes match expectation.

Go / no-go gate: both smoke tests must pass before full sweeps.

## 7. Expected results

**Predicted throughput ceilings** (from hw_bench_20260424 CXL seq_write
= 22 GB/s, rand_read = 26 GB/s):

| value_size | Bytes/UPDATE | BW-limited A ceiling | Bytes/READ | BW-limited C ceiling |
|-----------:|-------------:|---------------------:|-----------:|---------------------:|
| 8 (inline, iter-3) | 24 | — (latency-bound 15-17 Mops/s) | 128 | — (latency-bound 65 Mops/s) |
| 8 (pooled, new baseline) | 72 (bucket+pool_CL) | — (latency-bound, ≈ iter-3 ±5 %) | 192 | — (latency-bound) |
| 256 | 320 | **68 Mops/s** | 384 | **67 Mops/s** |
| 512 | 576 | **38 Mops/s** | 640 | **40 Mops/s** |
| 1024 | 1 088 | **20 Mops/s** | 1 152 | **22 Mops/s** |

**Predicted observed peaks** (assuming protocol C scales to 50–70 % of
BW ceiling under current flusher design):

| value_size | A peak | B peak | C peak |
|-----------:|-------:|-------:|-------:|
| 8 (pooled baseline) | 14–17 Mops/s | 45–50 | 60–65 (≈ iter-3) |
| 256 | 12–18 Mops/s | 30–40 Mops/s | 35–45 Mops/s |
| 512 | 8–12 | 20–28 | 22–28 |
| 1024 | 5–8 | 12–18 | 14–18 |

**Crossover hypothesis**: around 512 B, throughput becomes
value-size-limited rather than protocol-limited; multi-flusher or
per-drain-cycle bump optimisations stop helping.

## 8. Risks / open questions (for user review)

### 8.1 Single-path vs dual-path backcompat — **decided: dual-path**

Rationale for dual-path (§4.4 "Semantics"):
- **Data interpretation integrity.** If iter-4 shows A regressing
  from 17 → 10 Mops/s, a reader cannot distinguish "larger values"
  from "forced pool routing of u64 path". Dual-path keeps the u64
  path bit-identical to iter-3; all regressions are attributable
  to the new path alone.
- **A/B protocols untouched.** A and B share `cxl_hashtable.h` and
  `cxl_batch_ring.h`. With dual-path, A/B continue to run the
  inline-u64 path via the wrapper — no A/B code change required.
- **Maintenance cost.** The fork is 3 lines at the entry of
  insert/update/search, not a fork through the critical section.
  Unit tests cover both paths. Acceptable.

Name mangling: the slot field `blk_off` replaces `value` but is the
same 8 bytes at the same offset. Inline-u64 callers treat those
bytes as a u64 value; pooled callers treat them as a pool offset.
The hashtable doesn't care — only store-class methods interpret the
field. **No `#ifdef` needed; no A/B change needed.**

### 8.2 Apples-to-apples baseline — **decided: add kv8 pooled sweep**

Rationale: without this, iter-3 → iter-4 throughput deltas on
kv256/512/1024 conflate two variables (layout + size). With the
kv8 pooled sweep as anchor:
- kv8 pooled vs iter-3 16-B inline: layout refactor cost (should be
  ±5 %, else we have a regression to investigate before kv>8 results
  are usable)
- kv8 pooled vs kv256 pooled: pure value-size effect at same layout
- kv256/512/1024: size scaling curve at same layout

Budget: +30 min sweep time. Total iter-4 budget: 8–9 h.

### 8.3 Compile-time vs runtime value_size — **decided: runtime env var**

Rationale:
- Memcpy of 256/512/1024 B at runtime vs compile-time: ~50–100 ns
  difference. Swamped by CXL µs-scale latency (noise <5 %).
- Single binary, four sweeps; compile once, deploy once.
- Future size sweeps (2 KB, 4 KB, skewed distributions) only need
  new env values, no re-compile.
- `block_size_` is fixed at `attach()` time; `blk_off` semantics
  remain consistent. No ABI / layout surprises.

### 8.4 Lazy free — **decided: stub + document**

Interface present (`free_lazy(uint64_t off)`), implementation empty
(pushes onto a never-drained vector or no-op). Rationale:
- Peak allocation for the four sweeps: N_keys × value_size. With
  YCSB default N_keys = 1 M keys × 1 KB = **1 GiB peak per host**.
  Pool sized to 2 GiB (2× safety). No leak risk within scope.
- Real sweep workloads don't grow the key set, only churn values
  at existing keys → peak = footprint of all currently-live blocks
  (one per key), not total ops processed.
- iter-5 implementation will revisit with a ref-count + per-drain
  GC scheme; flagged in iteration_note.

### 8.5 Block pool per-host or shared — **decided: per-host segments**

Per-host segments eliminate allocator contention. Trade-off is 2×
memory commit vs shared pool. At 2 GiB per host × 2 hosts = 4 GiB
total within 512 GiB devdax — trivial. No revisit within iter-4.

### 8.6 Cache-on for value blocks — **decided: track bucket cache**

DRAM cache extension caches value blocks **only for currently
cached buckets**. Footprint: `N_cached_buckets × value_size`.
At 128 k cached buckets × 1 KB = 128 MiB DRAM — fine. Eviction
follows bucket cache eviction (coupled). **No separate LRU /
admission policy for value blocks** (scope).

## 9. Deliverables checklist

On completion:

- [ ] `src/cxl_kv_blockpool.{h,cc}` committed (with `free_lazy` stub)
- [ ] `src/cxl_hashtable.h` slot field renamed (value → blk_off)
- [ ] `src/cxl_kv_ops_C.{h,cc}` new API + dual-path (u64 wrapper
      bypasses pool; variadic always uses pool)
- [ ] `src/cxl_batch_ring.{h,cc}` field renamed (new_value → blk_off,
      same bytes)
- [ ] `tests/cxl_ycsb_runner.cc` buffer gen + env-var value_size +
      region sizing
- [ ] `src/CMakeLists.txt` add blockpool source (no macro, runtime-only)
- [ ] `tests/cxl_blockpool_test.cc` unit test (NEW)
- [ ] 4 sweep directories: `docs/g34_scaling_ycsb_C_only_<ts>_kv{8,256,512,1024}/`
- [ ] `docs/scaling_ycsb_runs_index.md` updated with 4 new rows
- [ ] `docs/iter4_variable_kv_summary_<date>.md` consolidated results
      (with kv8-vs-iter3 regression-check as gate 0)
- [ ] Cross-size comparison plot: throughput vs value_size (log x axis),
      for A/B/C/F at T=32/64/86 (grouped bars per T)
- [ ] Cross-size latency comparison plot: p50 + p99 vs value_size

## 10. Commit plan (squash-able)

1. `[kv-varlen] Phase-1: CxlKvBlockPool primitive + unit test`
2. `[kv-varlen] Phase-2: slot field rename (value → blk_off, bytes unchanged)`
3. `[kv-varlen] Phase-3: protocol C dual-path (u64 wrapper bypass + variadic API via pool)`
4. `[kv-varlen] Phase-4: protocol C read path + DRAM cache extension for value blocks`
5. `[kv-varlen] Phase-5: batching ring field rename (new_value → blk_off)`
6. `[kv-varlen] Phase-6: YCSB runner value buffer + runtime FUSEE_VALUE_SIZE env`
7. `[kv-varlen] Phase-7: four 80-run sweeps at value_size 8/256/512/1024`
8. `[kv-varlen] Phase-8: cross-size analysis + iteration notes`

---

## User review points — status table

| # | Point | Claude recommendation | User decision |
|---|-------|----------------------|---------------|
| 1 | Scope (C only?) | C only | ✅ confirmed |
| 2 | Back-compat path | Dual-path (u64 wrapper bypasses pool) | ⏳ pending |
| 3 | Add kv8 baseline sweep | Add it (needed for layout/size separation) | ⏳ pending |
| 4 | Value size switch mechanism | Runtime env var (single binary) | ⏳ pending |
| 5 | Lazy free | Stub now, implement iter-5 | ⏳ pending |
| 6 | Work budget | 8–9 h total (7–8 h impl + 4 sweeps) | ⏳ pending |

**Please sign off or counter-propose on points 2–6 before Phase-1 starts.**

This doc has been rewritten assuming all Claude recommendations are
accepted. If any is rejected, §4 and §8 need re-edits.
