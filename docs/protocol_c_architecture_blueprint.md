# Protocol C — Architecture Blueprint

> **Living document.** Update at the end of every iter whose code change
> touches the write/read/flusher path, the bucket / slot lock table, or
> the micro-batching ring.
>
> **Sister doc**: `docs/design_goals.md` is the "why" (LRC definition,
> RAPs, anti-patterns); **this** is the "what + how" (current
> implementation shape).
>
> **Sister blueprint**: `docs/protocol_a_architecture_blueprint.md` —
> Protocol A is the strict-A linearizable variant (sharded ownership,
> cross-host invalidate). C is the **Lazy Release Consistency (LRC)**
> variant: every host can write every key, per-bucket lock serializes
> writers, readers seqlock-retry on a per-bucket `write_epoch`.
>
> **Snapshot point**: end of iter-5 C-track work (2026-04-25, when
> focus transitioned to Protocol A — see memory
> `project_protocol_c_closed_protocol_a_open.md`). All code paths
> documented here are the ones that survive in `src/cxl_kv_ops_C.cc`
> at the iter-11A commit (e4c682b, 2026-05-11).

This doc has two layers and one cross-cutting reference:
- **Part I — System overview**: plain language, no code.
- **Part II — Per-stage pseudo-code dictionary**: indexed by stage tag
  (W1..W6, R1..R4, F1..F5). When a probe trace says "stage W4 p99 =
  18 µs", you open Part II §W4 and immediately see what W4 does in
  sub-steps + which CXL/DRAM primitives are involved.
- **Part III — Cross-cutting reference**: CXL primitive cost cheat
  sheet, synchronization contracts, known failure modes, build-time
  feature matrix.

---

# Part I — System Overview

## I.1 Single-tier topology (no DRAM authoritative state)

```
                   shared CXL Type-3 device (/dev/dax0.0, 512 GiB)
                   ┌──────────────────────────────────────────────┐
                   │  • Hashtable buckets       (single copy)     │
                   │  • BucketLockTable / SlotLockTable           │
                   │       per-bucket {mutex, write_epoch}        │
                   │       per-slot variant adds 7 slot mutexes   │
                   │       + route_seq (Phase-2.5)                │
                   │  • KvBlockPool (optional, iter-4 var-KV)     │
                   │  • OpLog        (optional, recovery)         │
                   └──────────────────────────────────────────────┘
                            ↑                    ↑
                   load/store via clflushopt + sfence/mfence
                            ↑                    ↑
            ┌───────────────┴────┐    ┌──────────┴──────────┐
            │  host 0 (g3)       │    │  host 1 (g4)        │
            │  per-host DRAM:    │    │  per-host DRAM:     │
            │   • Optional       │    │   • Optional        │
            │     DRAM cache     │    │     DRAM cache      │
            │     (per-process,  │    │     per-process     │
            │     epoch-validated│    │     epoch-validated │
            │     read fast path)│    │     read fast path) │
            │   • MicroBatch ring│    │   • MicroBatch ring │
            │     (DRAM, opt-in) │    │     (DRAM, opt-in)  │
            └────────────────────┘    └─────────────────────┘
```

**One tier, two roles**:
- **CXL** = single source of truth for **all** state (key→value pairs,
  the per-bucket `write_epoch`, optional oplog, optional pool). Writers
  publish; readers seqlock-retry against `write_epoch`.
- **DRAM (per host)** = optional, purely **read-side** L1: cached copy
  of bucket cachelines validated by `cached_epoch_[idx] == cxl_epoch`.
  No writer ever publishes through DRAM; writes go straight to CXL
  under the bucket / slot lock.

**Key difference from Protocol A**:
- No sharding (`ShardingTable`). Every host can write every key. The
  per-bucket lock serializes cross-host writers via the shared LFM /
  ticket mutex in `BucketLockEntry::mutex`.
- No sharer tracking (`SlotDirectory.sharer_bitmap`), no
  cross-host invalidate messaging (no InvalRing), no register-then-fill
  (no ReadRing / ReadStaging).
- No sender / receiver threads. Optionally one or more **flusher**
  threads when micro-batching is enabled (Phase-3).
- `KvCacheBucket::epoch` (Protocol A's L2 tag) is replaced by the
  bucket-lock-entry's `write_epoch`, which lives **on CXL** (peer host
  can read it via flush+mfence).

## I.2 The roles (per host)

C has far fewer named threads than A. There are no senders, no
receivers, no dispatcher.

| # | Role | Count per host | What it owns | CPU | Spawn site |
|---|---|---|---|---|---|
| 1 | **Worker** | T (1..64) | Issues KV ops (`insert/update/remove/search`). Each worker calls `lock_table_.lock(bucket_idx, host_id, num_hosts)` for serialization; reads do not take the lock. | cpu 0..(T-1) (pinned in tests; unpinned in microbench harness) | Each is a fork-child process or a `std::thread` of the test harness. |
| 2 | **Flusher** | 0–8 (configurable; `FUSEE_NUM_FLUSHERS`) | Drains the per-host MicroBatch ring (Phase-3 UPDATE batching). Each flusher owns a partition `bucket_idx % N == my_id` and a dirty-queue shard. ONE epoch bump per drained batch (amortized over up to K entries). | unpinned by default | `start_flusher()`; only spawned on the primary client (host-id-0 client-0) per host, since the ring is host-local MAP_SHARED. |
| 3 | (Implicit) primary client process | 1 | Calls `attach(init_region=true)` to zero the bucket array + lock table; calls `enable_batching(init_region=true)` to zero the MicroBatch region; starts flusher threads. Other client processes attach with `init_region=false` and spin until `init_done = 1`. | n/a | The first client started per host. |

**No deadlock cycle to break.** Because writers never send a message
to another host (they hold the cross-host LFM lock instead), there is
no analog to Protocol A's "ForwardResponder might invalidate a peer
which might be holding the directory lock waiting for ForwardResponder
to free a slot" — C's writer cycle is just `lock → CoW publish →
bump epoch → unlock`.

## I.3 Data layout cheat sheet

| Region | Tier | Sharing | Purpose | Approx. size |
|---|---|---|---|---|
| **Hashtable** | CXL | one copy | Authoritative key→value map. B buckets × S=7 slots × 16 B/slot. Same `CxlKvBucket` / `CxlKvSlot` layout as Protocol A — the slot is 16 B `{key, value}` regardless of protocol. | `B × S × 16 B` (~8 MB at B=65536) |
| **BucketLockTable** | CXL | one copy | One `BucketLockEntry` per bucket: `{mutex, write_epoch (cacheline_u64), staging_scratch (cacheline_u64)}`. **Default**: LFM mutex (`shm_mutex_t` from `~/cxl_shm_profiling/locks/lfm_lock.h`). Build flag `FUSEE_USE_TICKET_LOCK=1` swaps in `ticket_mutex_t` for bounded-wait benchmarks. `staging_scratch` is unused by C (Protocol B uses it). | `B × 192 B` (~12 MB at B=65536, mutex+epoch+scratch each 1 cacheline) |
| **SlotLockTable** (Phase-2b alt) | CXL | one copy | When `FUSEE_PER_SLOT_LOCK=1` is built in, BucketLockTable is replaced by `SlotLockEntry` per bucket: `{slot_mutexes[7], write_epoch, staging_scratch, route_seq (Phase-2.5)}`. Writers serialize only against same-slot writers on the same bucket; the per-bucket `write_epoch` is still the reader seqlock tag. **Default OFF**. | `B × 576 B` (7 slot mutexes + 3 cacheline metadata) |
| **KvBlockPool** | CXL | partitioned: H segments, each owned by one host | Optional (iter-4 variadic-length values). Activated by `store.set_blockpool(bp)`. For `value_len ≤ 8` the slot inline path is unchanged; for `value_len > 8` an offset into the pool is stored in `slot.value`. **No 4-B header prefix** (unlike Protocol A's iter-9A pool layout) — value bytes live directly at `blk_off` and the reader passes `out_cap` to `blockpool_->read()`. | workload-dependent |
| **OpLog** | CXL | one copy | Optional; activated by `store.enable_oplog(log)`. Mutating ops call `oplog_->begin(kind, key, bucket_idx, slot_idx, old_value, new_value)` BEFORE the publish, `oplog_->commit(idx)` AFTER. `recover_from_oplog()` rescans the log on attach and replays InProgress entries via the public `insert/update/remove` API. | log-sized (typically MB-scale) |
| **DRAM bucket cache** (`cache_buckets_` + `cache_epoch_`) | DRAM | per-process private | Activated by `enable_dram_cache(true)`. `cache_buckets_[idx]` is a full `CxlKvBucket` copy; `cache_epoch_[idx]` records the CXL `write_epoch` at the time the copy was taken. Reader fast path: load CXL epoch (1 cacheline), compare; on match, scan the DRAM copy without flushing slot cachelines. | `B × sizeof(CxlKvBucket)` per process (~7 MB at B=65536) |
| **MicroBatch ring** (`MicroBatchRing`) | **DRAM** `MAP_SHARED \| MAP_ANONYMOUS` (host-local) | Activated by `enable_batching(shm_base, shm_bytes, K, T_flush_us, init, num_flushers)`. Per-bucket `{append_cursor (atomic), queued (atomic u8), flush_cursor}`; per-bucket ring of K `RingEntry`; `BatchRingHeader` with `dq[kMaxFlushers=8]` MPSC dirty queues. **Lives in DRAM (not CXL)** because peer host never reads it — it batches THIS host's UPDATEs only. Saves the ~3 µs CXL roundtrip per append. | sized at attach time as a function of K |
| **Latency-decomp probes** | TLS | per-thread | When `-DFUSEE_LATENCY_DECOMP=1`, every mutating op emits `DECOMP_REC(stage, t0, t1)` covering `Lock / Scan / Publish / Epoch / Unlock / Total`. Stored TLS, dumped on thread exit. | trivial |

## I.4 Read path (overview)

**LRC reader contract** (per `docs/design_goals.md` Option C
definition): a successful `search()` returns a value that was the
committed value of `key` at **some instant** during the scan. Not a
consistent snapshot across the scan window — between the start of the
bucket scan and the end of the scan, a concurrent writer may have
committed a different value to a different slot. As long as the
returned `(key, value)` was atomically valid at some t ∈ [scan_start,
scan_end], LRC is satisfied.

Three logical outcomes:
1. **DRAM cache HIT, epoch fresh** → scan DRAM copy, no CXL flush
2. **DRAM cache MISS / epoch stale / cache disabled** → flush bucket + scan CXL
3. **Key not found** → return -1

Stage tags (R1..R4):

```mermaid
sequenceDiagram
  participant W as Worker (caller)
  participant CACHE as DRAM cache (per-process)
  participant LOCKE as BucketLockEntry (CXL)
  participant CXL as CXL bucket
  W->>+CACHE: R1: enter search(K)
  CACHE->>+LOCKE: load cxl_epoch (1 LD-CXL)
  alt cache_enabled and cxl_epoch == cached_epoch
    CACHE-->>-W: R2_dram_hit: scan DRAM copy (no flush); return
  else miss / stale
    W->>+LOCKE: R3: load e1 = write_epoch
    W->>+CXL: R4: flush_line(slots[0]); flush_line(slots[4]); mfence
    CXL-->>-W: scan 7 slots in L1 (now fresh from CXL)
    W->>CACHE: (if enabled) refresh cache_buckets_[idx] + cache_epoch_[idx] = e1
    W-->>-W: R5: return found value or -1
  end
```

**Phase-2.4 read-singleshot** (current implementation): the read path
does ONE epoch load + ONE bucket flush+scan, with NO epoch revalidation
at the end of the scan. Justification (in source comment): x86 aligned
u64 loads are atomic, so reading a slot's `key` and `value` words is
each torn-free. A racing writer that mutates a slot mid-scan can yield
`(old_key, new_value)` or `(new_key, old_value)`, but the returned
value is still one that was committed at some instant. LRC accepts
this; strict-A would not.

**Phase-2.6 flush-collapse**: a `CxlKvBucket` is 7 slots × 16 B = 112
B → 2 cachelines. The reader issues 2 `flush_line` calls
(`&slots[0]`, `&slots[4]`) to cover all 7 slots, then one `mfence`.

## I.5 Write path (overview)

C has three write-path flavors selectable at build / run time:

| Build / flag | Lock granularity | Epoch bumps per op | Notes |
|---|---|---|---|
| Default (per-bucket lock) | bucket-wide LFM (or ticket) | 1 per op | Simplest; writer holds the bucket lock for the whole insert/update/remove + epoch bump. |
| `FUSEE_PER_SLOT_LOCK=1` | per-slot ticket lock (Phase-2b) | 1 per op | Writer's critical section serializes only against other writers targeting the SAME slot. Per-bucket `write_epoch` is still the reader seqlock tag. UPDATE pre-scans add a `route_seq` snapshot to skip the under-lock re-verify when the bucket's slot layout hasn't changed since the unlocked scan (Phase-2.5). |
| `enable_batching(K, T, num_flushers)` runtime opt-in | NO lock for UPDATE fast path | 1 per drained batch (amortized over up to K entries) | UPDATE only — INSERT and DELETE still take the lock. UPDATE writers `append()` into a DRAM ring; flusher thread drains, applies to CXL, bumps epoch once. **The biggest C-track perf lever.** |

```mermaid
sequenceDiagram
  participant W as Worker (writer)
  participant LOCKE as BucketLockEntry (CXL)
  participant CXL as CXL bucket
  participant LOG as OpLog (CXL, opt)
  W->>W: W1: bucket_idx = fnv1a_u64(key) % B
  W->>+LOCKE: W2: lock_table_.lock(idx, host_id, num_hosts) [LFM acquire]
  W->>+CXL: W3: flush_line(&slots[0]); flush_line(&slots[4]); mfence; scan 7 slots
  alt INSERT
    note over W,CXL: find first empty slot; abort if dup or full
  else UPDATE / DELETE
    note over W,CXL: find slot matching key; abort if not found
  end
  W->>LOG: (if enabled) oplog->begin(kind, ...); record log_idx
  W->>+CXL: W4: publish slot — for INSERT/UPDATE write value+key with flushes; for DELETE store key=EMPTY
  W->>LOCKE: W5: bump_epoch(entry) — atomic fetch_add + flush + sfence
  W->>LOG: (if enabled) oplog->commit(log_idx)
  W->>CACHE: (if enabled) refresh own DRAM cache slot
  W-->>-LOCKE: W6: unlock
```

**iter-2 ordering note**: in the per-slot-lock variant, `unlock_slot`
runs BEFORE `bump_epoch`. The slot lock only serializes same-slot
writers; the per-bucket `write_epoch` is what readers seqlock on.
Letting the next writer enter the slot critical section while we bump
the bucket epoch saves ~2 µs from the hot-slot critical section.

## I.6 Cross-host coordination

C has **no cross-host messages**. Hosts coordinate through three CXL
artifacts and three only:

| Artifact | Producer | Consumer | Synchronization |
|---|---|---|---|
| `BucketLockEntry.mutex` | any writer (any host) | any writer | LFM lock (or ticket); single-host blocked-on-mutex bookkeeping in `~/cxl_shm_profiling/locks/`. Cross-host acquisition is the C-track's only blocking inter-host primitive. |
| `BucketLockEntry.write_epoch` | writer post-publish | any reader | Reader does `CACHELINE_LOAD(&entry->write_epoch)` (which is flush_line + mfence + LD); writer does `__atomic_add_fetch + flush_line + sfence`. |
| CXL bucket cachelines | writer under lock | any reader | Reader flushes both cachelines + mfence before scanning. Writer flushes after publish + sfence. The `write_epoch` change is the cross-host visibility signal — readers caught between writer's slot-flush and writer's epoch-flush still re-scan if they observe the new epoch later. |

## I.7 Key design choices (one-liner each)

| Choice | One-line rationale |
|---|---|
| **All hosts can write every key** | C is the LRC point; no sharding means no ownership transfer / hot-shard problem |
| **CXL bucket lock (LFM)** | Cross-host serializer is one primitive; LFM is the sibling repo's tuned cross-host mutex |
| **Per-bucket `write_epoch`** | Cheapest seqlock tag a peer host can read; just a `CACHELINE_LOAD` |
| **Phase-2.4 read-singleshot** | LRC permits "value at some instant during scan", so the post-scan epoch revalidation is unnecessary work |
| **Phase-2.6 flush-collapse** | Bucket = 2 cachelines, so 2 flush_line calls suffice (vs flushing each slot's cacheline) |
| **Phase-2b per-slot lock** (opt-in) | Multiple writers on different slots of a hot bucket don't block each other; per-bucket epoch still serves readers |
| **Phase-2.5 `route_seq`** | UPDATE readers can skip the under-lock re-verify when no INSERT/DELETE has changed slot layout since the unlocked pre-scan — large optimization on read-heavy + write-light workloads |
| **Phase-3 micro-batching (DRAM ring)** | UPDATE fast path bypasses the per-bucket lock entirely; one epoch bump amortizes K writes; ring is host-local DRAM (peer never reads it) so no CXL roundtrip per append |
| **`unlock` BEFORE `bump_epoch`** | Slot lock only serializes same-slot writers; releasing it before the epoch atomic shortens the hot-slot critical section |
| **OpLog is opt-in** | Recovery is a separate concern; tests that don't care leave it null |
| **DRAM cache per-process (not MAP_SHARED)** | No concurrent writer in the same process (process owns its own cache); much simpler than Protocol A's L2 |

---

# Part II — Per-Stage Pseudo-Code Dictionary

## II.0 Notation

**Primitive abbreviations** (same as Protocol A; see
`docs/protocol_a_architecture_blueprint.md §II.0` for the full table).
Healthy baselines are derived from `cxl_primitive_bench` measurements
on g3/g4 (TSC=2.0 GHz).

**Decomp stage tags** (used by `DECOMP_REC` when
`-DFUSEE_LATENCY_DECOMP=1`): `Lock`, `Scan`, `Publish`, `Epoch`,
`Unlock`, `Total`. These are the per-op time decomposition that the
`cxl_latency_decomp_C` micro-bench produces and that
`docs/path_decomp_spec.md` references for C-track analysis.

---

## II.1 Worker write path (W1..W6)

Source: `src/cxl_kv_ops_C.cc :: insert/update/remove`.

### W1 — Entry to mutator (insert / update / remove)

```
W1: enter insert/update/remove(key [, value]):
  if key == EMPTY: return -1
  idx = fnv1a_u64(key) % num_buckets_
  bucket = &buckets_[idx]
  entry = lock_table_.entry(idx)
```

**Primitives**: 1× FNV-1a hash, 2 pointer-arith.
**Healthy baseline**: ~15 ns CPU.

### W2 — Acquire bucket / slot lock

```
W2: per-bucket lock:
  lock_table_.lock(idx, host_id_, num_hosts_)   // LFM acquire (cross-host blocking)
  DECOMP_REC(Lock, t0, t1)

OR — when FUSEE_PER_SLOT_LOCK=1, UPDATE/DELETE:

W2a: snapshot route_seq, unlocked pre-scan:
  seq_before = load_route_seq(entry)
  flush_line(&slots[0]); flush_line(&slots[4]); mfence
  target = (first slot matching key) or -1
  if target < 0: return -1
  lock_table_.lock_slot(idx, target)            // ticket lock acquire
  if load_route_seq(entry) != seq_before:
    flush_line(&slots[target].key); mfence
    if slots[target].key != key:
      unlock_slot(idx, target); retry  // bounded by kInsertRetries=8
  DECOMP_REC(Lock, t0, t1)
```

**Primitives (per-bucket variant)**: 1× LFM acquire; on cross-host
contention this is the dominant cost.
**Healthy baseline**: ~50 ns uncontested LFM acquire on same host.
Cross-host contended: 5 µs - tens of ms depending on queue depth
(see Protocol A `III.1` spinlock-contention table for analogous
scaling — LFM has a tighter ramp because the wait-queue is on CXL).

**Primitives (per-slot variant)**: 1× ticket lock acquire + 2×
`route_seq` LD-CXL (pre + post) + on layout-change branch a 1×
`slots[target].key` flush+LD-CXL. Uncontested adds ~600 ns over the
per-bucket cost for the two extra LD-CXLs.

→ `src/cxl_kv_ops_C.cc` insert/update/remove top of each `for attempt`
loop.

### W3 — Bucket scan (with flush-collapse)

```
W3: bucket scan:
  flush_line(&bucket->slots[0])
  flush_line(&bucket->slots[4])
  full_fence()
  (linear search 7 slots for matching key OR first empty)
  DECOMP_REC(Scan, t1, t2)
```

**Primitives**: 2× FLUSH (covers all 7 slots' cachelines), 1× MFENCE,
up to 7× LD on now-fresh cachelines (~10 ns total in L1).
**Healthy baseline**: ~150 ns (2× 66 ns flushes + mfence + register
loop).
**Sync contract**: under the bucket / slot lock, `target_slot` is
exclusively ours to mutate.

### W4 — Publish slot (CoW under lock)

For UPDATE (value-only mutation, no slot re-routing):

```
W4 (UPDATE):
  bucket->slots[target].value = value
  flush_line(&bucket->slots[target].value)
  store_fence()
  DECOMP_REC(Publish, t2, t3)
```

For INSERT (writing key+value into an empty slot):

```
W4 (INSERT, publish_slot helper):
  slot->value = value
  flush_line(&slot->value); store_fence()
  slot->key   = key
  flush_line(&slot->key);   store_fence()
  DECOMP_REC(Publish, t2, t3)
```

For DELETE:

```
W4 (DELETE):
  slot->key = kEmptyKey
  flush_line(&slot->key); store_fence()
  DECOMP_REC(Publish, t2, t3)
```

**Primitives**: 1-2× ST-CXL, 1-2× FLUSH, 1-2× SFENCE.
**Healthy baseline**: ~150 ns for UPDATE / DELETE (single cacheline);
~300 ns for INSERT (value flush + key flush, two-step publish so a
racing reader cannot see `(new_key, old_value)`).
**Sync contract**: value flush ordered before key flush on INSERT
ensures any reader that sees `slot.key == key` also sees the
post-flush `slot.value`.

### W5 — Bump per-bucket write_epoch

```
W5: bump_epoch (atomic):
  new_epoch = __atomic_add_fetch(&entry->write_epoch.value, 1, ACQ_REL)
  flush_line(&entry->write_epoch); store_fence()
  DECOMP_REC(Epoch, t3, t4)
```

**Primitives**: 1× RMW-CXL (atomic fetch_add on CXL line) + 1× FLUSH
+ 1× SFENCE.
**Healthy baseline**: ~1.4 µs (the `cxl_primitive_bench` measured
`CXL atomic fetch_add + flush + sfence` cost is 1.435 µs p50).
**Sync contract**: this RMW + flush + sfence is what makes the new
slot value cross-host visible. Readers re-load `write_epoch`,
detect change, re-flush + re-scan slots.
**iter-2 ordering**: in per-slot-lock variant, `unlock_slot()` runs
BEFORE `bump_epoch` — letting the next same-slot writer enter while
this one's epoch bump is in flight (their writes serialize on the
bucket epoch anyway).

**Phase-3 micro-batching path SKIPS W5**: the UPDATE fast path
`append()`s a ring entry, then the flusher does ONE `bump_epoch`
per drained batch (amortized over up to K entries). See II.3 below.

### W6 — Unlock + (optional) OpLog commit + DRAM cache refresh

```
W6: cleanup:
  if oplog_: oplog_->commit(log_idx)
  if cache_enabled_:
    cache_buckets_[idx].slots[target] = {key, value}
    cache_epoch_[idx] = new_epoch
  lock_table_.unlock(idx, host_id_)              // per-bucket variant only
  // (per-slot variant unlocked at W4 end, before W5)
  DECOMP_REC(Unlock, t4, t5)
  DECOMP_REC(Total,  t0, t5)
  return 0
```

**Primitives**: optional 1× OpLog commit (CXL ST), 1× ST-DRAM cache
refresh, 1× LFM release.
**Healthy baseline**: ~100 ns (DRAM cache + lock release).

**Healthy baseline total** (W1→W6 UPDATE, per-bucket lock, no OpLog,
no DRAM cache, KV=8): ~2 µs typical. **The 1.4 µs epoch bump
dominates**; this is why Phase-3 micro-batching exists.

---

## II.2 Worker read path (R1..R5)

Source: `src/cxl_kv_ops_C.cc :: search()`.

### R1 — Entry to search

```
R1: enter search(key, *out):
  if key == EMPTY: return -1
  idx = fnv1a_u64(key) % num_buckets_
  entry = lock_table_.entry(idx)
  bucket = &buckets_[idx]
```

### R2_dram_hit — DRAM cache fast path (cache_enabled_ only)

```
R2_dram_hit: try DRAM cache:
  cached = cache_epoch_[idx]
  if cached != UINT64_MAX:
    cxl_epoch = CACHELINE_LOAD(&entry->write_epoch)   // 1 flush+mfence+LD
    if cxl_epoch == cached:
      // No CXL flush of slot cachelines; scan the DRAM copy.
      for s in 0..6:
        if cache_buckets_[idx].slots[s].key == key:
          *out = cache_buckets_[idx].slots[s].value; return 0
      return -1
```

**Primitives**: 1× LD-CXL (`write_epoch` cacheline only), 1× LD-DRAM
scan of cached bucket.
**Healthy baseline**: ~700 ns (one CXL roundtrip on the epoch cacheline
+ DRAM scan).
**Sync contract**: if `cxl_epoch == cached`, no writer has touched
this bucket since we last refreshed the cache → the cached
`(key, value)` pairs were valid at the moment we wrote `cached_epoch_`
and remain valid now → LRC satisfied (returned value was committed at
some instant in [last refresh, now]).

### R3 — Snapshot epoch (read-singleshot)

```
R3: snapshot CXL epoch:
  e1 = CACHELINE_LOAD(&entry->write_epoch)
```

Per Phase-2.4: this single epoch read is also our "consistency tag" —
LRC does NOT require us to re-load and compare after scanning. If we
wanted strict-A here we would loop "read epoch → scan → re-read
epoch → retry on mismatch", but that's exactly what Protocol A's
`cache_pool_lookup` seqlock-CAS reader does (see Protocol A
blueprint §II.2 R2).

### R4 — CXL bucket scan + (opt) cache refresh

```
R4: flush + scan:
  flush_line(&bucket->slots[0])
  flush_line(&bucket->slots[4])
  full_fence()
  if cache_enabled_:
    cache_buckets_[idx] = *bucket          // 112 B DRAM copy
  for s in 0..6:
    if bucket->slots[s].key == key:
      captured = bucket->slots[s].value
      found = true; break
  if cache_enabled_:
    cache_epoch_[idx] = e1                 // associate copy with the epoch
```

**Primitives**: 2× FLUSH-CXL, 1× MFENCE, up to 7× LD on now-fresh
cachelines, optional 1× 112-B DRAM memcpy + 1× ST-DRAM.
**Healthy baseline (cache_enabled_)**: 1.5-2 µs (2× 66 ns flushes +
mfence + register scan + 112-B DRAM memcpy).
**Healthy baseline (cache_disabled_)**: ~150-300 ns (just flushes +
scan).
**LRC contract**: x86 aligned u64 loads are atomic per slot field;
returned `value` is a committed value at some instant in [scan_start,
scan_end] even if a concurrent writer races (we may see
`(old_key, new_value)` for ONE slot but the BREAK on key match
ensures the returned `value` was paired with `old_key == key`).

### R5 — Return

```
R5: return:
  if found: *out = captured; return 0
  else:     return -1
```

**Healthy baseline total** (R1→R5 UPDATE-light workload,
cache_enabled_, hit): ~700 ns. Cache_miss path: ~1.5-2 µs. Both
substantially under Protocol A's L2 hit (~5-15 µs at large KV)
because C does not memcpy 1024 B of value bytes — the slot is the
authoritative source and only 8 bytes are returned per op.

→ `src/cxl_kv_ops_C.cc:559-615`

---

## II.3 Phase-3 micro-batching UPDATE path (B1..B4)

Source: `src/cxl_kv_ops_C.cc::update()` (top branch when
`batch_enabled_`), `src/cxl_batch_ring.h::MicroBatchRing::append`.

Active when `enable_batching(...)` was called pre-fork on the primary
client. Only UPDATE uses this path — INSERT and DELETE still go
through W1..W6 (taking the bucket / slot lock + per-op epoch bump).

### B1 — Unlocked pre-scan for target slot

```
B1: unlocked scan:
  flush_line(&bucket->slots[0]); flush_line(&bucket->slots[4]); mfence
  target = -1
  for s in 0..6:
    if bucket->slots[s].key == key: target = s; break
  if target < 0: return -1
```

**Primitives**: 2× FLUSH + 1× MFENCE + register scan.
**Healthy baseline**: ~150 ns (Phase-2.6 flush-collapse).

### B2 — Append to per-bucket DRAM ring

```
B2: ring append:
  c = &cursors[bucket_idx]
  while ring full at this bucket: pause; ring_full_waits++
  pos = c->append_cursor.fetch_add(1, ACQ_REL)
  e = &ring[bucket_idx * K + pos % K]
  e->slot_idx = target
  e->new_value = value
  __atomic_store_n(&e->flags, 1, RELEASE)  // publish entry
```

**Primitives**: 1× RMW-DRAM (cursor fetch_add) + 1× ST-DRAM (entry
fields) + 1× ST-DRAM (flags release).
**Healthy baseline**: ~50-100 ns. **Zero CXL roundtrips on this path
(ring is DRAM).**
**Sync contract**: flusher consumer spins on `e->flags == 1` (release
acquire pair) before reading entry fields.

### B3 — Enqueue bucket on dirty-queue (if first writer this drain cycle)

```
B3: first-writer enqueues:
  expected = 0
  if c->queued.compare_exchange_strong(expected, 1):
    // We won the CAS; push bucket_idx onto dq[bucket_idx % num_flushers].
    shard = &header->dq[bucket_idx % num_flushers]
    tpos = shard->dq_tail.fetch_add(1, ACQ_REL)
    shard->dq_slots[tpos % capacity] = bucket_idx
```

**Primitives**: 1× CAS-DRAM (queued flag) + 1× RMW-DRAM (dq_tail) +
1× ST-DRAM.
**Healthy baseline**: ~50 ns on the CAS-winning path; ~5 ns when
another writer already enqueued this bucket.
**Sync contract**: at most one entry per (bucket, drain cycle) in
the dirty queue — caps queue inflation under sustained workload.

### B4 — Read-your-writes DRAM cache refresh (opt)

```
B4: own cache refresh:
  if cache_enabled_:
    cache_buckets_[idx].slots[target].value = value
    // (no cache_epoch_ bump — flusher will bump CXL epoch later;
    //  we keep our local view tagged with the old cached epoch
    //  and rely on the cxl_epoch == cached check at next read)
```

UPDATE caller then returns; CXL is not touched on the fast path.
The actual CXL publish + epoch bump happens in the flusher thread
(§II.4).

→ `src/cxl_kv_ops_C.cc:258-298` (batch fast path)

---

## II.4 Flusher loop (F1..F5)

Source: `src/cxl_kv_ops_C.cc :: flusher_loop / drain_bucket`,
`src/cxl_batch_ring.h :: MicroBatchRing` (cursors, dirty-queue).

One or more flusher threads per host, started by `start_flusher()` on
the primary client. Each thread owns partition `bucket_idx %
num_flushers == my_id` and a `DirtyQueueShard` (MPSC).

### F1 — Dirty-queue pop

```
F1: pop one dirty bucket from my shard:
  dq = &hdr->dq[my_id]
  tail = dq->dq_tail.load(ACQUIRE)
  head = __atomic_load_n(&dq->dq_head, ACQUIRE)
  if head >= tail: (no work; goto F4)
  idx = dq->dq_slots[head % capacity]
  __atomic_store_n(&dq->dq_head, head+1, RELEASE)
```

**Primitives**: 1-2× LD-DRAM atomic.
**Healthy baseline**: ~50 ns.

### F2 — Drain bucket's ring window

```
F2: drain ring:
  c = &cursors[idx]
  append_raw = c->append_cursor.load(ACQUIRE)
  flush_cur  = c->flush_cursor          // single-writer (this flusher)
  // Cap drain window at flush_cur + K (avoid deadlock with writers
  // that already claimed a pos >= flush_cur + K and are spin-waiting
  // for flush_cursor advance before publishing their flag).
  append_end = min(append_raw, flush_cur + K)
  for p in [flush_cur, append_end):
    e = &ring[idx * K + p % K]
    while __atomic_load_n(&e->flags, ACQUIRE) == 0: pause  // wait for writer's RELEASE
    // (with FUSEE_BATCH_MERGE_SAME_KEY: collapse to per-slot latest)
    bucket->slots[e->slot_idx].value = e->new_value      // ST-DRAM-then-flush via F3
```

**Primitives**: 1× LD-DRAM atomic on `append_cursor` + per-entry spin
on `flags` + up to K ST-DRAM into bucket cachelines.
**Healthy baseline**: ~50-200 ns per drained entry (dominated by the
spin on writer's `flags` if writer pre-empted, otherwise CPU-bound).
**Optional**: `-DFUSEE_BATCH_MERGE_SAME_KEY=1` collapses to
last-writer-wins per slot — at most 7 slots in a bucket, so a tiny
fixed-size `int64_t latest_pos[7]` beats a hashmap. Trade-off: extra
pass over the window in exchange for fewer slot writes.

### F3 — Single bucket publish to CXL

```
F3: bucket publish:
  flush_line(&bucket->slots[0])
  flush_line(&bucket->slots[4])
  store_fence()
```

**Primitives**: 2× FLUSH-CXL + 1× SFENCE.
**Healthy baseline**: ~150 ns (Phase-2.6 flush-collapse — 2 flushes
cover the 7-slot bucket regardless of how many of the K entries hit
distinct slots).

### F4 — ONE bump_epoch amortized over K entries

```
F4: amortized epoch bump:
  new_epoch = bump_epoch(lock_table_.entry(idx))     // 1.4 µs RMW-CXL + flush + sfence
```

This is the central win of Phase-3 batching. K UPDATEs cost 1×
`bump_epoch` instead of K of them. At K=16 the per-op cost drops from
~1.4 µs to ~90 ns (epoch amortized). At K=64 to ~22 ns.

### F5 — Clear flags + advance flush_cursor + clear queued

```
F5: cleanup:
  for p in [flush_cur, append_end):
    __atomic_store_n(&ring[idx*K + p%K].flags, 0, RELEASE)
  __atomic_store_n(&c->flush_cursor, append_end, RELEASE)
  c->queued.store(0, RELEASE)   // re-arm dirty-queue enqueue for next writer
```

After F5 the flusher loops back to F1 (or, if dirty queue is empty
and 5 ms have passed since last work, scans its partition for
buckets with pending entries — a fallback for the rare case where a
writer enqueues but the flusher's dirty queue is full).

**Idle policy**: 10 µs `nanosleep` between empty queue checks → flusher
does not pin a core under low load. **Shutdown**: `stop_flusher()`
sets `hdr->stop`; each flusher does a final partition scan to drain
residuals before joining.

→ `src/cxl_kv_ops_C.cc:684-826`

---

# Part III — Cross-Cutting Reference

## III.1 CXL primitive cost cheat sheet (shared with Protocol A blueprint)

See `docs/protocol_a_architecture_blueprint.md §III.1`. The same
measured values apply (same g3/g4 testbed):

- `clflushopt + sfence` ≈ 66 ns
- LD-CXL post-flush+mfence ≈ 630 ns
- ST-CXL + flush + sfence ≈ 14 ns
- CXL atomic `fetch_add + flush + sfence` ≈ **1.4 µs** (this is W5
  and F4 dominant cost)
- LFM mutex acquire same-host uncontested ≈ ~50 ns; cross-host
  contended scales similarly to spinlock T-curve

## III.2 Synchronization contracts summary

| Lock / channel | Holder | Waiter pattern | Bound |
|---|---|---|---|
| `BucketLockEntry::mutex` (LFM or ticket) | one writer at a time (across all hosts) | LFM spin + futex-like wait on CXL queue | Cross-host contended: spinlock-like ramp; LFM has bounded fairness vs raw spin |
| `SlotLockEntry::slot_mutexes[7]` (ticket, FUSEE_PER_SLOT_LOCK build) | one writer per (bucket, slot) | ticket spin | bounded by number of contending threads on the same slot |
| `BucketLockEntry::write_epoch` (cacheline_u64) | writer post-publish | reader does CACHELINE_LOAD | n/a — single 8-B atomic, no waiter blocked |
| `MicroBatchRing.cursors[idx].append_cursor` | any same-host worker | producer DRAM RMW; spin only if ring full | bounded by K and flusher drain rate |
| `MicroBatchRing.cursors[idx].queued` | first writer this drain cycle | CAS 0→1 | n/a — succeeds for at most one writer per cycle |
| `DirtyQueueShard.dq_tail` / `dq_head` | any same-host worker producer / single owning flusher | DRAM RMW on tail, plain ld/st on head | bounded by `kDirtyQueueCapacity = 8192` |
| `OpLog` | any writer (any host) | atomic RMW on tail; flushes recovery slot | log-sized |

## III.3 Failure modes glossary

| Symptom | Code | Meaning | Recovery |
|---|---|---|---|
| `insert` returns -2 | duplicate key | bucket scan found existing matching key under lock | caller treats as already-inserted (YCSB harness re-issues) |
| `insert` returns -1 | bucket full | all 7 slots occupied with non-matching keys | application must rehash or accept partial load — currently no expansion |
| `update` / `remove` returns -1 | key not found | bucket scan completed without match | application abort or treat as no-op |
| `insert` returns -1 after `kInsertRetries=8` (per-slot lock variant) | repeated lose-the-slot races | INSERT raced with another inserter that took our candidate empty slot 8 times in a row | very rare — treat as fully contested bucket; application retries |
| `update` returns -1 after `kInsertRetries=8` (per-slot lock variant) | racing INSERT+DELETE moved key | route_seq mismatch + under-lock re-verify failed 8× in a row | rare; treat as not-found |
| `recover_from_oplog()` returns N > 0 | crash mid-op | InProgress entries found and replayed via redo function | idempotent: re-applying succeeded ops returns -2 (insert dup), which the redo function maps to "already applied" |
| Ring-full backpressure under Phase-3 batching | `ring_full_waits_` counter increments | flusher cannot keep up with append rate | increase K, decrease T_flush_us, or add flushers |
| Flusher waits forever in F2 inner spin | writer pre-empted between `fetch_add` and `flags=1 RELEASE` | writer must publish flag for flusher to proceed | the cap at `flush_cur + K` prevents the flusher from blocking on slots whose writer is itself waiting for flush_cursor to advance — deadlock-free |

## III.4 Build-time / runtime feature matrix

| Switch | Default | Effect |
|---|---|---|
| `-DFUSEE_PER_SLOT_LOCK=1` | OFF | Use `SlotLockTable` (per-slot ticket lock + `route_seq`) instead of `BucketLockTable` (per-bucket LFM). Phase-2b experiment. |
| `-DFUSEE_USE_TICKET_LOCK=1` | OFF | Swap LFM mutex → ticket mutex in `BucketLockEntry`. Bounds worst-case fairness; baseline benchmarks have used this for tail-latency comparisons. |
| `-DFUSEE_BATCH_MERGE_SAME_KEY=1` | OFF | Flusher's `drain_bucket` collapses K entries to per-slot last-writer-wins before applying. Reduces redundant slot stores at the cost of one extra ring pass. |
| `-DFUSEE_LATENCY_DECOMP=1` | OFF | Enable `DECOMP_REC(stage, t0, t1)` probes in every mutating op (Lock / Scan / Publish / Epoch / Unlock / Total). |
| Runtime `enable_dram_cache(true)` | OFF | Activate per-process bucket cache (`cache_buckets_`, `cache_epoch_`). Reader fast path bypasses CXL flush. |
| Runtime `enable_oplog(log)` | NULL | Attach a `CxlOpLog` for crash recovery. Mutators begin/commit around each op. |
| Runtime `enable_batching(K, T_us, init, num_flushers)` | OFF | Activate Phase-3 UPDATE micro-batching. Only INSERT/DELETE keep the per-op lock + epoch path. |
| Runtime `set_blockpool(bp)` | NULL | iter-4 variadic-length values. value_len ≤ 8 inline; value_len > 8 stored in pool, slot.value = blk_off. |

---

# Update Protocol

This doc must be updated:
1. **End of every iter** that ships a code change to
   `src/cxl_kv_ops_C.{h,cc}`, `src/cxl_bucket_lock.{h,cc}`,
   `src/cxl_batch_ring.{h,cc}`, or `src/cxl_oplog.{h,cc}`. The iter
   summary doc must include a "Blueprint updates" section listing
   which Part I / II / III sections changed and why.
2. **When stage tags shift** (e.g., new B5 sub-step added between B4
   and F1, or a stage is split). Probe scripts and the decomp tags
   cross-reference these.
3. **When healthy baseline changes** by > 2× for any stage.
4. **When a failure mode is added or eliminated**.

Snapshot history kept in iter summary docs (each summary references
the blueprint version at iter-end).

**Latest version**: end of iter-5 C-track work (2026-04-25). C-track
was placed on pause when focus transitioned to Protocol A (see
`docs/iters/iter5_summary.md` / project memory). Code paths
documented above remain in `src/cxl_kv_ops_C.cc` at the
iter-11A repo state (commit e4c682b, 2026-05-11) — no
post-iter-5 changes were made to C.

## iter-1 through iter-5 changes summary (for context)

- **iter-1**: per-bucket LFM lock + write_epoch seqlock; baseline.
- **iter-2**: `unlock` BEFORE `bump_epoch` (per-slot variant); ~2 µs
  saved on hot-slot critical section.
- **iter-2.4**: read-singleshot (no post-scan epoch revalidation); LRC
  permits this.
- **iter-2.5**: `route_seq` added to `SlotLockEntry`; UPDATE pre-scan
  snapshot lets the under-lock re-verify be skipped when no
  INSERT/DELETE has changed slot layout.
- **iter-2.6**: flush-collapse — 2 `flush_line` calls cover all 7
  slots of a bucket regardless of which slot the op targets.
- **iter-2b**: `FUSEE_PER_SLOT_LOCK` build flag — per-slot ticket
  lock variant. Per-bucket `write_epoch` still serves readers.
- **iter-3**: Phase-3 micro-batching. Per-host DRAM
  `MicroBatchRing` + 1-8 flusher threads. UPDATE fast path
  bypasses the bucket lock; one epoch bump per drained batch. The
  biggest single perf lever on C-track.
- **iter-4**: variadic-length values via `CxlKvBlockPool`. Inline u64
  for value_len ≤ 8; pool offset stored in slot.value for value_len
  > 8.
- **iter-5**: multi-flusher V2 + M1 measurement work — per-host CXL
  write 12.5 GB/s ceiling identified; A peak 19.35 Mops/s
  (kv256/N=2) was the standout but fell short of the 25 Mops/s bar
  Protocol C aspires to. **Workload-A bottleneck reclassified as
  hot-bucket producer-bound under Zipf, not flusher-rate-bound**
  → C-track work transitioned focus to Protocol A iter-1A
  (per memory `project_protocol_c_closed_protocol_a_open.md`).
