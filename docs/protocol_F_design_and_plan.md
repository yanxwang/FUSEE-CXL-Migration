# Protocol F: FUSEE-on-CXL Baseline — Design and Implementation Plan

**Status**: draft (pre-implementation) | **Created**: 2026-06-07
**Purpose**: Implement a CXL-native KV store that mirrors FUSEE's data structures and read/write protocols as closely as the CXL 2.0 fabric permits, to serve as a fair experimental baseline for Protocol A in the FUSEE-CXL paper.
**Companion docs**: [project_motivation_and_design_rationale.md](project_motivation_and_design_rationale.md) §5–§8 (architectural placement of Protocol F vs Protocol A); [iters/iter16A_summary_20260521.md](iters/iter16A_summary_20260521.md) (latency primitives used for design cost estimates).

---

## 1. Motivation

### 1.1 The experimental question

The FUSEE-CXL paper argues that **static key-range partition + software directory** (Protocol A) is the right reconstruction of cross-host coherence when the CXL 2.0 fabric provides none. To make that argument empirically, we need a baseline that takes the alternative position: **any host writes any bucket**, structurally close to RDMA-era FUSEE.

Protocol C (the existing iter-1 through iter-5 codebase) was a first attempt at this, but with three design drifts away from FUSEE that make it an unfair comparison:

1. **Inline (key, value) in 16 B slot** vs FUSEE's 8 B pointer-only slot
2. **Per-process whole-bucket DRAM mirror cache** vs FUSEE's per-key (slot_addr, kvpair_addr) index cache
3. **Lazy Release Consistency (LRC)** reads vs FUSEE's Linearizable reads

Protocol C is closed for production work and these drifts cannot be cleanly retrofitted via compile flags. Protocol F is a fresh, FUSEE-faithful implementation written alongside Protocol C, not replacing it.

### 1.2 Comparison target

Protocol F is the "no static partition" baseline. Concretely:

- Same data structures as FUSEE (8 B slot, separate KV pair region, DRAM index cache with adaptive bypass)
- Same Linearizability guarantee
- Same write strategy (any host writes any slot, optimistic-style concurrency)
- **One unavoidable difference**: CXL 2.0 has no cross-host atomic CAS, so a software lock substitutes for FUSEE's `RDMA_CAS`. This is the experimental variable the paper measures: how much does this substitution cost?

The paper's contrast is then:
- Protocol A: rebuild coherence via static partition (avoid cross-host serialization entirely)
- Protocol F: rebuild coherence via software lock (serialize at slot granularity)

---

## 2. Design

### 2.1 Data structures

All three core structures match FUSEE's RACE-hashing layout. Lock state is the only addition, and it lives in a physically separate region.

#### 2.1.1 Slot (8 B, FUSEE-shaped)

```cpp
struct CxlFuseeSlot {
  uint64_t packed;
  //  [0..47]  blk_off       — 48-bit byte offset into KV-pair pool (CXL-base-relative)
  //  [48..55] size_class    — 8-bit selector for KV-pair pool size class
  //  [56..63] fingerprint   — low 8 bits of fnv1a(key); for fast slot reject
  //
  // packed == 0  means empty slot.
};
static_assert(sizeof(CxlFuseeSlot) == 8);
```

Field layout matches FUSEE's `(ptr:48, fp:8, len:8)` shape. We rename `len` to `size_class` because our KV-pair pool uses discrete size classes; this is a naming difference, not a structural one.

#### 2.1.2 Bucket (128 B, 2 cachelines)

```cpp
constexpr int kCxlFuseeSlotsPerBucket = 14;

struct CxlFuseeBucket {
  CxlFuseeSlot slots[14];  // 14 * 8 = 112 B
  uint64_t     pad[2];     // 16 B pad → 128 B total
};
static_assert(sizeof(CxlFuseeBucket) == 128);
```

14 slots per bucket comes from FUSEE's typical RACE-hashing associativity. Bucket aligned to 128 B (two cachelines) for RDMA-style flush patterns and CXL cacheline boundaries.

#### 2.1.3 KV pair (immutable, separate region)

```cpp
// Logical layout — physical bytes depend on size_class.
// For inline u64 KV (16 B record):
struct CxlFuseeKvPairInline {
  uint64_t key;
  uint64_t value;
};

// For variable-length KV (size_class >= 1):
struct CxlFuseeKvPairVarHdr {
  uint64_t key;
  uint32_t value_len;
  uint32_t reserved;
  // value bytes follow inline, up to size_class block capacity
};
```

KV pairs are **immutable** after write. UPDATE allocates a new KV pair at a new address rather than overwriting. This is what makes FUSEE's lockless reader path correct, and we preserve it for the same reason (see §3, Correctness).

KV pair contains the **full key** so readers can verify (key, value) coherence after following a slot pointer.

#### 2.1.4 KV pair pool with free bitmap (FUSEE §4.4)

```cpp
// One block = unit of coarse-grained allocation (FUSEE uses 16 MB).
// Block header: per-object free bitmap. Object size = size_class block size.
struct CxlFuseeBlockHeader {
  uint32_t block_id;
  uint32_t size_class;
  uint64_t num_objects;
  // Followed by free bitmap: (num_objects + 7) / 8 bytes, one bit per object.
  // bit == 0 → in use; bit == 1 → freed.
};
```

Free bitmap is updated by clients with `atomic_fetch_or` + flush_line (to make the bit visible cross-host). Background reclaim thread scans bitmap and adds freed addresses to a client-local free list (DRAM). Allocation pops from the local free list. Per FUSEE §4.4.

#### 2.1.5 SlotLockTable (external, FUSEE has no equivalent)

```cpp
// One LFM lock entry per slot in the bucket array.
// Layout chosen to keep slot data uncontaminated by lock state.
struct CxlFuseeLockEntry {
  CachelineU64 lock_state;   // LFM ticket / state field
};

class SlotLockTable {
  CxlFuseeLockEntry *entries_;  // num_buckets * 14 entries
  // attach(), lock_slot(bucket_idx, slot_idx), unlock_slot(bucket_idx, slot_idx)
};
```

Physically separate from the bucket array. Readers never touch it. Writers touch the lock entry for their target slot only.

### 2.2 Physical CXL memory layout

```
CXL devdax region:
┌────────────────────────────────────────────────────┐
│ Header (cookie, num_buckets, num_hosts, init_done) │
├────────────────────────────────────────────────────┤
│ SlotLockTable (NEW — FUSEE has no analog)          │
│   - num_buckets * 14 lock entries                  │
├────────────────────────────────────────────────────┤
│ Bucket array (FUSEE-shaped)                        │
│   - num_buckets * sizeof(CxlFuseeBucket)           │
├────────────────────────────────────────────────────┤
│ KV pair pool (FUSEE-shaped)                        │
│   - Multiple blocks, each with free bitmap header  │
│   - Block size classes: e.g., 16 B / 256 B /       │
│     512 B / 1024 B records                         │
└────────────────────────────────────────────────────┘
```

### 2.3 DRAM index cache (FUSEE §4.6)

```cpp
struct CxlFuseeIndexCacheEntry {
  uint64_t key;                     // hash table key for this entry
  uint64_t slot_addr;               // CXL address of the slot last seen for this key
  uint64_t kvpair_addr;             // CXL address of the KV pair last seen for this key
  uint32_t access_cnt;              // §4.6 adaptive bypass: total accesses
  uint32_t invalid_cnt;             // §4.6 adaptive bypass: cache-stale count
  bool     bypass;                  // when set, skip cache fast path for this key
};

// Hashmap keyed by key, fixed-capacity LRU eviction.
// Per-process DRAM (not shared across hosts, not on CXL).
class CxlFuseeIndexCache {
  // lookup(key) -> entry* or nullptr
  // insert(key, slot_addr, kvpair_addr)
  // record_hit(key)
  // record_miss(key)  -> increments invalid_cnt, recomputes ratio, sets bypass if >threshold
  // is_bypass(key)    -> consult bypass flag
};
```

Adaptive bypass per FUSEE §4.6:
- Each cache entry counts `access_cnt` and `invalid_cnt`.
- `invalid_cnt` increments when the cache hit's `cached_kvpair_addr` does not match the current `slot.packed.blk_off`.
- When `invalid_cnt / access_cnt > threshold` (default 0.5; tunable via env), entry is marked `bypass`. Subsequent reads on that key skip the fast path and go straight to bucket scan.
- A write-heavy key that becomes read-heavy will see `access_cnt` keep climbing while `invalid_cnt` stops — the ratio decays below threshold, and `bypass` clears automatically. No explicit invalidation needed.

### 2.4 Write path

#### 2.4.1 INSERT (workload-A's I path; FUSEE Figure 9 INSERT, single-replica)

```
1. compute bucket_idx = hash(key) % num_buckets
2. allocate KV pair from local free list (size class derived from value_len):
   2a. pop addr from CxlFuseeKvPairPool::alloc(size_class)
   2b. write {key, value} to *addr on CXL
   2c. flush_line(addr) + sfence  ← KV pair fully persisted before slot publish
3. scan bucket for empty slot (or fp match → return -2 dup):
   3a. flush_line(bucket.slots[0]) + flush_line(bucket.slots[8]) + mfence
   3b. for s in 0..13:
       if slot.packed == 0 → candidate empty slot
       if slot.fp == fp(key):
         follow slot.blk_off → read KV pair → if pair.key == key → return -2 dup
4. lock the candidate slot: SlotLockTable.lock_slot(bucket_idx, candidate)
5. under-lock re-verify:
   5a. flush_line + mfence on the bucket
   5b. re-scan: if candidate still empty AND no dup landed in other slots:
       continue to publish
   5c. otherwise: unlock, retry from step 3 (bounded retries)
6. publish slot: write {packed = pack(addr, size_class, fp)} to bucket.slots[candidate]
   flush_line(&slot) + sfence
7. unlock slot: SlotLockTable.unlock_slot(bucket_idx, candidate)
8. update DRAM index cache: insert(key, &slot, addr)
```

Note: KV pair is written first, then made reachable via slot.packed. Order matters for the reader-side immutability argument (see §3).

#### 2.4.2 UPDATE (workload-A's U path; FUSEE Figure 9 UPDATE, single-replica)

```
1. compute bucket_idx
2. find target slot for key:
   2a. lookup DRAM index cache. if hit (and !bypass): use cached slot_addr.
   2b. otherwise: flush + scan bucket, follow fp-matching slots until pair.key == key.
   2c. if not found → return -1
3. allocate NEW KV pair (do NOT overwrite old):
   3a. new_addr = pool.alloc(size_class)
   3b. write {key, new_value} to *new_addr; flush_line; sfence
4. lock target slot
5. re-verify under lock:
   5a. flush + read target slot, follow slot.blk_off, read pair, verify pair.key == key
   5b. if key no longer matches (concurrent INSERT/DELETE moved it): unlock + retry
   5c. else: capture old_addr = slot.blk_off
6. publish new slot value: slot.packed = pack(new_addr, size_class, fp)
   flush_line(&slot) + sfence
7. unlock slot
8. free old KV pair:
   8a. pool.free(old_addr) → atomic_fetch_or on free bitmap bit
   8b. flush bitmap byte + sfence
9. update DRAM index cache: insert(key, &slot, new_addr)
```

Old KV pair is freed (bit set in bitmap), but its memory is NOT cleared. Concurrent readers holding the old slot.blk_off can still safely read the pair until the address is reclaimed and reused. See §3 for the safety argument.

#### 2.4.3 DELETE

```
1-2. as UPDATE
3. lock target slot
4. re-verify under lock
5. capture old_addr = slot.blk_off
6. publish empty: slot.packed = 0; flush_line(&slot) + sfence
7. unlock slot
8. free old KV pair (same as UPDATE step 8)
9. evict from DRAM index cache
```

### 2.5 Read path (SEARCH)

#### 2.5.1 Fast path (cache hit, not bypassed)

```
1. cache_entry = index_cache.lookup(key)
2. if cache_entry exists and not bypass:
   2a. parallel issue:
       slot_state = CXL_LOAD(cache_entry.slot_addr)
       cached_pair = CXL_LOAD(cache_entry.kvpair_addr)  // load whole pair via memcpy + key+value
       // (parallel here means issue both before either completes; the CPU's
       // out-of-order machinery + memcpy + prefetch achieves this in practice)
   2b. extract slot_blk_off from slot_state.packed
   2c. if slot_blk_off == cache_entry.kvpair_addr (the offset, not the absolute address):
         // Cache hit valid.
         if cached_pair.key == key:
           index_cache.record_hit(key)
           return cached_pair.value
         // else fp collision / stale eviction-rewrite — fall through to slow path
   2d. else:  // cache stale: writer UPDATEd the slot since cache was populated
       index_cache.record_miss(key)
       fresh_pair = CXL_LOAD(slot_blk_off)
       if fresh_pair.key == key:
         index_cache.insert(key, cache_entry.slot_addr, slot_blk_off)
         return fresh_pair.value
       // else: fp collision — fall through to slow path
3. slow path (cache miss / bypass / cache-cleared by collision)
```

#### 2.5.2 Slow path (cache miss or bypassed)

```
3. compute bucket_idx = hash(key) % num_buckets
4. flush_line(bucket.slots[0]) + flush_line(bucket.slots[8]) + mfence
5. for s in 0..13:
   if slot.packed == 0: continue
   if slot.fp != fp(key): continue
   pair = CXL_LOAD(slot.blk_off)
   if pair.key == key:
     index_cache.insert(key, &slot, slot.blk_off)
     return pair.value
6. return -1 (not found)
```

#### 2.5.3 Reader-side correctness with KV pair reclamation

A reader that follows a slot.blk_off, reads the KV pair, and verifies `pair.key == requested_key` is correct **even if the KV pair's address was freed and reused for a different key in between**:
- If the address was reused for a different key K', the pair at that address now has `key == K'`.
- Reader's `pair.key == requested_key` check fails (K' ≠ requested_key).
- Reader treats it as a miss and falls through to slow path or returns not-found.

This is exactly FUSEE's safety mechanism. No epoch / RCU / quiescence needed.

### 2.6 KV pair GC (FUSEE §4.4 free-bitmap mechanism)

```cpp
// Per-block, free bitmap header. 1 bit per object.
// bit = 0 → in use; bit = 1 → freed.

// Free path (called from UPDATE/DELETE):
void CxlFuseeKvPairPool::free(uint64_t addr) {
  block, obj_idx = locate(addr);
  byte_idx = obj_idx / 8;
  bit_mask = 1 << (obj_idx % 8);
  __atomic_fetch_or(&block.free_bitmap[byte_idx], bit_mask, __ATOMIC_ACQ_REL);
  flush_line(&block.free_bitmap[byte_idx]);
  sfence();
}

// Reclaim thread (per-process, background):
void reclaim_loop() {
  while (running_) {
    for each block this client has allocated from:
      flush_region(&block.free_bitmap, bitmap_size); mfence();
      for each set bit:
        addr = block_base + obj_idx * obj_size;
        // Clear the bit, atomic_fetch_and ~mask, flush, sfence
        push_to_local_free_list(addr);
    sleep_for(reclaim_interval_ms);  // e.g. 10 ms
  }
}

// Alloc path (called from INSERT/UPDATE):
uint64_t CxlFuseeKvPairPool::alloc(uint8_t size_class) {
  if (local_free_list[size_class].nonempty())
    return local_free_list[size_class].pop();
  // local free list exhausted: claim a fresh block from the pool
  return allocate_fresh_block(size_class);
}
```

For correctness of reader interactions with reclaim, see §2.5.3 above.

---

## 3. Correctness: Linearizability

Protocol F provides Linearizable single-key reads and writes. The argument has three pieces.

### 3.1 Write linearization point

For each write operation, the linearization point is the moment of the final flushed slot store (UPDATE/INSERT step 6 above, after the slot is updated and the cacheline is flushed). LFM lock ensures total order on writes to the same slot. Cross-slot writes (different bucket / different slot of the same bucket) have no inherent order — that is consistent with the per-key correctness specification.

### 3.2 Read linearization point

For each read operation, the linearization point is the moment of reading `slot.packed`. The value of slot.packed at that moment determines which KV pair will be reported (the one at slot.packed.blk_off).

### 3.3 Key invariant: KV pair immutability

A KV pair, once written and made reachable via a slot, is never modified at the same address. UPDATE allocates a new KV pair at a new address; the old KV pair remains intact until reclamation, and even then any rewrite at the same address uses a different key (which the reader's `pair.key` verification catches).

Therefore, given a slot.packed value `s`:
- The KV pair at `s.blk_off` contains *some* (key, value) pair that was written before s was published to the slot.
- A reader following s.blk_off reads that exact (key, value) pair, no torn state.
- If the reader's key check passes, the value returned is the one the writer published.
- If the key check fails (because the address was reused), the read is treated as a miss — the read's linearization point lies before *any* of the colliding writers wrote.

### 3.4 No retry / seqlock needed

Unlike a hypothetical "no-immutability + seqlock" design, Protocol F does not need:
- Epoch read-validate-retry loops
- Reader-side locks
- Read fences beyond what's needed to flush stale cachelines

Because: slot is 8 B aligned (atomic single load), and KV pairs are immutable (no torn writes). The reader's correctness argument reduces to "I read a slot, I followed it, I verified my key matches", with each step atomic at the hardware level we already have.

### 3.5 The role of LFM lock

LFM lock serializes the *writers* on the same slot. It does not participate in read-side correctness. Its only job is to enforce the write linearization total order (§3.1) that CXL 2.0's missing cross-host atomic CAS cannot provide directly.

This is the exact mechanism FUSEE achieves with RDMA_CAS: serialize writers per slot. The substitution is mechanism-different but semantics-equivalent.

---

## 4. Differences from FUSEE

A summary of where Protocol F deviates from a literal FUSEE port, with rationale:

| Feature | FUSEE | Protocol F | Reason for difference |
|---|---|---|---|
| Writer synchronization | RDMA_CAS (hardware) | LFM lock (software) | CXL 2.0 has no cross-host atomic CAS |
| Replication | 3 replicas + SNAPSHOT protocol | 1 replica | CXL data is physically singular; no MN-failure tolerance need |
| Lock state location | n/a (no locks) | External SlotLockTable | Keeps slot/bucket layout 100% FUSEE-shaped |
| CRC in KV pair | Yes (1 byte) | Not implemented initially | CXL fabric is not Byzantine; reader's key-verify is sufficient for our correctness model. (Easy to add if needed.) |
| Embedded operation log | Yes (per-KV-pair log entry for crash recovery) | Not in MVP; OpLog wired up only if benchmark needs recovery | Iter-4A+ Protocol A scaffolding can be reused; not on the critical path for the baseline comparison |
| Index cache eviction | Adaptive (paper doesn't detail eviction) | Bounded-size LRU | Concrete simple choice; not load-bearing for the comparison |

Everything else — slot layout, bucket layout, KV pair layout, GC mechanism, read path, write path structure, adaptive bypass — matches FUSEE.

---

## 5. Code organization

New files, all under [src/](../src/) and [tests/](../tests/):

| File | Purpose | Approximate scope |
|---|---|---|
| `src/cxl_fusee_bucket.h` | 8 B slot, 14-slot bucket, packed helpers | Small header (~80 LOC) |
| `src/cxl_fusee_kvpair_pool.h` | KV pair pool API (alloc/free/reclaim thread interface) | Small header |
| `src/cxl_fusee_kvpair_pool.cc` | Pool impl with free-bitmap + reclaim thread | Moderate |
| `src/cxl_fusee_index_cache.h` | Per-key index cache + adaptive bypass API | Small header |
| `src/cxl_fusee_index_cache.cc` | Hashmap, counters, bypass threshold | Moderate |
| `src/cxl_kv_ops_F.h` | `CxlKvStoreF` class declaration | Header |
| `src/cxl_kv_ops_F.cc` | insert / update / delete / search / attach implementation | Large (the protocol itself) |
| `src/CMakeLists.txt` | Add new source files; add `FUSEE_OPT_F` compile-time switch | Small edit |
| `tests/cxl_protocol_f_hashdiff_test.cc` | Cross-host Linearizable correctness test | Substantial |

Existing files **not** modified by Protocol F:
- `src/cxl_kv_ops_A.{h,cc}` (Protocol A untouched)
- `src/cxl_kv_ops_C.{h,cc}` (Protocol C frozen at commit `5c83965`)
- `src/cxl_hashtable.h` (used by A/C with 16 B inline slots; Protocol F uses its own bucket/slot types in `cxl_fusee_bucket.h`)

### 5.1 Compile-time switch

Following the convention in the project, the active protocol is selected via:
```
cmake -DCONSENSUS_OPT=FUSEE_OPT_F ...
```

The test binaries (`tests/cxl_ycsb_runner`, hash-diff tests) will dispatch to `CxlKvStoreF` when `FUSEE_OPT_F` is the active flag.

### 5.2 Runtime configuration knobs

| Knob | Default | Purpose |
|---|---|---|
| `FUSEE_F_CACHE_CAPACITY` | 131072 entries | DRAM index cache size (LRU-bounded) |
| `FUSEE_F_BYPASS_THRESHOLD` | 0.5 | Adaptive bypass invalid_ratio cutoff |
| `FUSEE_F_RECLAIM_INTERVAL_MS` | 10 | Background reclaim thread sleep |
| `FUSEE_F_POOL_BLOCK_SIZE_MB` | 16 | Coarse-grained block size (matches FUSEE §4.4) |

All env-var driven, no recompile to change.

---

## 6. Test plan

### 6.1 Single-host functional tests

A minimal smoke test that loads, inserts, updates, deletes, searches under a single process. Verifies API correctness against a `std::unordered_map` oracle.

### 6.2 Single-host concurrency (fork-based "2-host" simulation)

While g1 dax0.0 is broken, we cannot run real 2-host tests. The fork-based pattern used in `tests/cxl_kv_hashdiff_test.cc` (Protocol A correctness battery) simulates 2-host writers within one host via shared `MAP_SHARED|MAP_ANONYMOUS` region.

Test scenarios:
1. **Concurrent INSERT, distinct keys**: 2 processes insert disjoint key sets, verify no loss.
2. **Concurrent UPDATE, same key**: 2 processes UPDATE the same key concurrently, verify Linearizable outcome (one of the two writes wins; the other is ordered before or after consistently across all readers).
3. **Concurrent INSERT + DELETE, same key**: verify no data corruption and final state is one of the two operations' outcomes.
4. **Mixed read+write**: workload-A pattern (50/50 R/W) for 5 s; cross-host hash-diff at end must show identical state on both hosts.

### 6.3 Two-host hash-diff (once testbed is restored)

The Protocol A hash-diff infrastructure (`tests/protocol_a_v2_invariant_check.cc` and related) can be adapted. Test: 2 hosts run a workload (uniform, zipf-0.99, zipf-1.5), compute hash of all (key, value) pairs at end, must match across hosts and match a single-process oracle.

### 6.4 Performance baseline (for paper)

YCSB-A and YCSB-C sweeps, comparable cells to Protocol A:
- T ∈ {1, 2, 4, 8, 16, 32, 64}
- N ∈ {1, 2} hosts
- value size V ∈ {8, 256, 1024} bytes
- 5 reps per cell

This produces the Protocol-F-vs-Protocol-A comparison plot for the paper's evaluation section.

---

## 7. Implementation sequencing

Stages, to be done in order:

### Stage 1 — Data structures and pool

1. `cxl_fusee_bucket.h`: types, pack/unpack helpers, hash function.
2. `cxl_fusee_kvpair_pool.{h,cc}`: pool layout, alloc, free, free bitmap, reclaim thread (initially without thread, single-threaded compaction OK for first smoke).
3. Unit tests: pool alloc/free correctness, bitmap visibility across fork.

### Stage 2 — Index cache

4. `cxl_fusee_index_cache.{h,cc}`: hashmap, LRU eviction, access/invalid counters, bypass threshold.
5. Unit tests: hit/miss tracking, bypass toggle adaptation.

### Stage 3 — Protocol core

6. `cxl_kv_ops_F.{h,cc}`: `attach`, `insert`, `search` (slow path only — no cache fast path yet).
7. Single-host smoke test: insert N keys, search them all back, compare to oracle.
8. Add: cache fast path in `search`, verify with cache enabled vs cache cleared (results identical).
9. Add: `update`, `remove`, with old-KV-pair free.
10. Smoke test: full CRUD against oracle.

### Stage 4 — Concurrency

11. Add LFM lock acquisition in write path.
12. Fork-based 2-process concurrent INSERT test (distinct keys).
13. Fork-based 2-process concurrent UPDATE same key.
14. Fork-based 2-process mixed workload (workload-A), hash-diff at end.

### Stage 5 — Benchmarking integration

15. Wire `FUSEE_OPT_F` into `tests/cxl_ycsb_runner.cc`.
16. Run small smoke YCSB-A and YCSB-C on single host, sanity check throughput.
17. Once testbed restored: full sweep.

### Stage 6 — Reclaim thread + adaptive bypass tuning

18. Enable background reclaim thread; verify under long-running test no memory growth.
19. Bypass threshold sweep (0.3, 0.5, 0.7) on a zipf-skewed workload, pick reasonable default.

Stages 1–4 can be done with the testbed offline (fork-based testing). Stage 5 partially needs the testbed; Stage 6 fully needs it.

---

## 8. Open questions for the paper

These are paper-level questions, not implementation blockers. Listed here so they aren't lost.

1. **CRC in KV pair**: FUSEE includes a 1-byte CRC for crash-recovery integrity. We've deferred it. If the paper claims crash safety, we'll need to add it; if the paper only claims runtime correctness, key-verify alone suffices.
2. **OpLog integration**: FUSEE has an embedded operation log for client crash recovery. Protocol A's OpLog code (`src/cxl_oplog.{h,cc}`) can be reused. Decision deferred until paper scope is fixed.
3. **Adaptive bypass threshold**: FUSEE paper doesn't pin a value. We'll default to 0.5 and run a sensitivity sweep for the paper.
4. **Index cache capacity**: paper question — what fraction of working set should the cache cover for the comparison to be fair? Likely 100% of YCSB keyspace (~16 M keys at YCSB default) for the paper's main plots, plus a sensitivity sweep at smaller capacities.

---

## 9. References

- **[Shen FAST'23]** Shen et al., *FUSEE: A Fully Memory-Disaggregated Key-Value Store.* USENIX FAST 2023. Specifically §4.2 (RACE hashing), §4.4 (Two-Level Memory Management), §4.6 (Adaptive Index Cache), Figure 9 (workflows).
- [project_motivation_and_design_rationale.md](project_motivation_and_design_rationale.md) — overall paper-seed doc; Protocol F is the empirical anchor for the C4 = "software lock" branch (§6.1) of the design-choice tree.
- [iter16A_summary_20260521.md](iters/iter16A_summary_20260521.md) — primitive latency measurements (clflushopt+sfence ≈ 66 ns) used for cost-of-LFM estimates.
- Existing Protocol C codebase (commit `5c83965` and earlier) — reference for SlotLockTable mechanics and OpLog wiring patterns. Not directly inherited.
