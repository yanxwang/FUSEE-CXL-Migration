# Step-by-Step Explainer for `protocol_a_vs_c_path_comparison.pptx`

> Speaker notes for the 4 topology slides (slides 2-5). For each numbered
> step A1..A14 / A1r..A16r / C0..C6 / C1r..C5r, this doc gives:
>   - **What it does** (the concrete action)
>   - **Why** (which protocol invariant or coordination need)
>   - **Cost basis** (which CXL / DRAM primitive)
>   - **Source code** (file:line of the relevant code)
>
> Sweep cell context: workload-A, KV=1024 B, T=64 clients per host, 2 hosts.

---

# Slide 2 — Protocol A Write Path

**Operation**: host 0 worker calls `update(K, V)` where the owner host of K is host 1 and host 0 also has K cached.

## Stage map

```
host 0 worker  ─A1─►ForwardStaging ─A2─►WriteRing ─A3─►WriteEntry ─A4 spin───────────────A14◄ACK return
                                                                                      ▲
host 1 WriteReceiver ─A5◄poll── A6 W1/W2/W3 ── A7 I1 ── A8 I2 ─A9 spin── A13 W7..W12──┘
                                                          ▼               ▲
host 0 InvalReceiver ─A10 I3/I4── A11 I5 ── A12 I6 ACK────┘
```

## A1 — write V bytes → `ForwardStaging[0][1][slot]`  [1.1 µs est]

**What**: worker memcpy's the 1024-B value bytes into the per-(src, dst, slot) CXL staging arena at `fs_->slots[host_id_=0][owner=1][slot_idx]`, then flushes 16 cachelines + 1 sfence.

**Why**: C2 invariant — value bytes must NOT travel on the WriteRing (rings are control-only, 128-B entry). The WriteEntry only carries `(key, op_kind, value_len, staging_off=slot_idx)`; the actual bytes go through this separate arena. Same `slot_idx` is reused so receiver knows where to read.

**Cost basis**: 16 × `clflushopt` (~66 ns each = 1.06 µs) + 1 `sfence` (~14 ns).

**Source**: [src/cxl_kv_ops_A.cc:1158-1166](src/cxl_kv_ops_A.cc#L1158) (the `memcpy + flush_line loop` inside `forward_write_direct`).

## A2 — `WriteRing[0][1].tail.fetch_add(1)`  [1.4 µs est]

**What**: worker atomically claims its slot index in the cross-host WriteRing by incrementing the tail counter (which lives on a CXL cacheline shared with host 1), then flushes the tail cacheline and sfences so host 1's WriteReceiver can observe the new tail.

**Why**: SPSC ring per (src, dst) host pair — host 0 is the sole producer on WriteRing[0][1]. The atomic fetch_add serializes WriteEntry slot claims between same-host workers on this ring.

**Cost basis**: this is the **most expensive single CXL primitive** in the protocol — `cxl_primitive_bench` measured CXL `fetch_add + flush + sfence` at 1.435 µs p50, 1.7 µs p99 (CXL roundtrip required for peer visibility).

**Source**: [src/cxl_kv_ops_A.cc:1142-1144](src/cxl_kv_ops_A.cc#L1142).

## A3 — write `WriteEntry{key, op_kind, value_len, staging_off}` + flush + sfence  [0.1 µs est]

**What**: worker fills WriteEntry's cacheline 1 (req-side) with `(key, op_kind, value_len, staging_off=slot_idx, staging_gen=0)`, zero out `resp_op_id`, then `e->req_op_id.store(op_id, RELEASE)` — this is the "publish" of the request. Flush + sfence makes the entry visible cross-host.

**Why**: the receiver polls `req_op_id != 0` to detect a new request. The store of `req_op_id` must be the LAST store in the entry's line (release semantics) so that any reader who sees `req_op_id != 0` also sees the other fields.

**Cost basis**: 1× ST-CXL to cacheline 1 + 1× clflushopt + 1× sfence ≈ 80 ns + atomic_thread_fence overhead.

**Source**: [src/cxl_kv_ops_A.cc:1168-1180](src/cxl_kv_ops_A.cc#L1168).

## A4 — worker spin on `WriteRing.resp_op_id`  [~22 µs blocked]

**What**: worker enters `generic_spin_wait()`. Each loop iteration: flush cacheline 2 of the entry (resp side, separate from line 1 to avoid producer-consumer ping-pong per iter-9A Phase 2 fix), full_fence, load `resp_op_id`. If `== op_id` → ACK received → return status. If timeout 5 ms → return `-11`.

**Why**: synchronous request-response. The worker must block until host 1 finishes executing the write **and** all peer invalidates are ACKed — that's what makes A's write path linearizable.

**Cost basis**: this is NOT a primitive cost — it's WALL TIME spent waiting for the entire A5..A14 chain on host 1 + host 0's InvalReceiver to complete. ~22 µs measured for cross-host write with inval on workload-a kv=1024 T=64.

**Source**: [src/cxl_kv_ops_A.cc:67-98](src/cxl_kv_ops_A.cc#L67) (`generic_spin_wait` template), invoked from line 1182.

## A5 — WriteReceiver polls + reads staging  [1.5 µs est]

**What**: host 1's WriteReceiver thread (cpu 65, single thread, blue-pinned) is in its main loop polling all WriteRing[*][1].tail values. Sees host 0's tail moved → reads `WriteEntry.req_op_id`, gets `op_id`. Then issues `flush_line + LD-CXL` on each cacheline of `ForwardStaging[0][1][slot_idx].bytes` to pull value bytes into local L1.

**Why**: WriteReceiver is the **only thread that consumes** WriteRing[*][1] — its single-threaded poll loop drains by `head < tail` semantics. Staging bytes are read AFTER seeing `req_op_id != 0` (release acquire pair).

**Cost basis**: ring poll observes tail change (1 cacheline LD, ~700 ns due to MESI from host 0's recent write) + 16 cacheline flush+LD on staging (mostly latency-hidden by prefetch since CXL reads stream).

**Source**: [src/cxl_kv_ops_A.cc:1621-1656](src/cxl_kv_ops_A.cc#L1621) (`write_receiver_loop`) + [1489-1511](src/cxl_kv_ops_A.cc#L1489) (`write_handler`).

## A6 — `execute_write_local`: W1 hash+flush + W2 spinlock + W3 bitmap  [5.0 µs measured]

**What**: WriteReceiver enters [`execute_write_local`](src/cxl_kv_ops_A.cc#L166) acting **on behalf of host 0 worker**. Three probe-tagged sub-stages:
- **W1**: compute `bucket_idx = fnv1a_u64(key) % num_buckets`; `flush_line` both cachelines of the bucket; `full_fence`. Forces the bucket cachelines to be re-fetched cross-host so we see any concurrent peer writes.
- **W2**: linearly scan 7 slots looking for `key` match (UPDATE/DELETE) or first empty (INSERT). Then acquire `SlotDirectoryEntry.spinlock` — this is a host-local DRAM pthread_spinlock (NOT cross-host), used to serialize same-host writers on this slot.
- **W3**: re-flush bucket under lock (lock acquire may have been a long sync point); read `de->sharer_bitmap` from DRAM (cheap, hot cacheline).

**Why**: bucket flush is for cross-host coherence on the slot array. Spinlock serializes same-host workers (multiple workers on host 1 might write same slot). Bitmap tells us which peer hosts need invalidate.

**Cost basis**: W1 mean **4.5 µs** measured — dominated by hot Zipf bucket MESI ping-pong (the bucket is fetched fresh on every probe at T=64 because many cores touch the same bucket). W2 0.27 µs (uncontended DRAM spinlock). W3 0.19 µs.

**Source**: [src/cxl_kv_ops_A.cc:173-221](src/cxl_kv_ops_A.cc#L173).

## A7 — `InvalRing[1][0].tail.fetch_add(1)` (probe I1)  [1.65 µs measured]

**What**: WriteReceiver is now INSIDE the broadcast loop (W4-W6). For each bit set in `sharer_bitmap` excluding self, it calls [`send_invalidate_direct(target_host, key)`](src/cxl_kv_ops_A.cc#L1196) which atomically claims an InvalRing slot via `fetch_add` on `InvalRing[1][0].tail`, then flush+sfence.

**Why**: InvalRing is a **separate channel** from WriteRing — required to break a deadlock cycle (WriteReceiver running execute_write_local needs to send invalidates; if invalidate shared the WriteRing channel and consumer thread, circular wait → deadlock). InvalRing has its own consumer thread (InvalReceiver, cpu 69) per host.

**Cost basis**: same CXL `fetch_add + flush + sfence` ≈ 1.4 µs. Measurement 1.65 µs slightly higher due to MESI contention on the tail cacheline at T=64.

**Source**: [src/cxl_kv_ops_A.cc:1203-1206](src/cxl_kv_ops_A.cc#L1203).

## A8 — write `InvalEntry{key, req_op_id}` + flush + sfence (probe I2)  [3.7 µs measured]

**What**: WriteReceiver fills the InvalEntry slot — but FIRST has to wait for the slot to be free (previous occupant's `req_op_id == 0`). At T=64 with broadcast invalidates being frequent, slot-free wait can spin. Then writes `(key, req_op_id)` to cacheline 1, sets resp_op_id to 0 on cacheline 2, RELEASE fence, flush cacheline 1.

**Why**: slot wait-for-free is the (currently unbounded) wait that occasionally cascades into the iter-7A "bug-floor" — known issue, iter-8A capped only the post-publish ACK spin at 5 ms, not the wait-for-free spin.

**Cost basis**: measured 3.7 µs mean — likely mix of healthy (~80 ns for write+flush) and slot-busy wait (microseconds when ring is hot).

**Source**: [src/cxl_kv_ops_A.cc:1210-1225](src/cxl_kv_ops_A.cc#L1210).

## A9 — WriteReceiver spin on `InvalRing.resp_op_id`  [~1.9 µs blocked]

**What**: WriteReceiver is now blocked inside its `send_invalidate_direct` call, spinning on cacheline 2 of the InvalEntry. Each iter: flush line 2, full_fence, load `resp_op_id`. If `== op_id` → ACK from host 0 InvalReceiver → return. Timeout 5 ms = return `-11` (silent fail per iter-7A, fail-loud is iter-12A backlog).

**Why**: writer must observe peer cache invalidation BEFORE proceeding to W9 (the commit point) — that's how §I9 strict-A linearizability is preserved (the new value's commit can't be observed before the old cached value is marked stale on the peer).

**Cost basis**: wall time = A10+A11+A12 on host 0 InvalReceiver ≈ 1.9 µs healthy.

**Source**: [src/cxl_kv_ops_A.cc:1236-1267](src/cxl_kv_ops_A.cc#L1236).

## A10 — InvalReceiver poll + read entry (probe I3/I4)  [1.75 µs measured]

**What**: host 0's InvalReceiver thread (cpu 69, single thread) is polling `InvalRing[1][0].tail` in its own main loop. Sees new tail → flush+LD entry's `req_op_id` (cacheline 1) → gets `op_id` and `key`.

**Why**: InvalReceiver is **completely independent** of WriteReceiver — different thread, different ring, different cacheline. Crucially does NOT call execute_write_local and holds NO directory lock → breaks the iter-4A-redo deadlock cycle.

**Cost basis**: 1 flush + LD-CXL on tail (~700 ns) + 1 flush + LD-CXL on entry req (~700 ns) ≈ 1.4 µs. Measured 1.75 µs.

**Source**: [src/cxl_kv_ops_A.cc:1322-1358](src/cxl_kv_ops_A.cc#L1322) (`inval_receiver_loop`).

## A11 — `cache_pool_set_stale(K)`; `bucket_epoch++` (probe I5)  [0.03 µs measured]

**What**: InvalReceiver calls `cache_pool_set_stale(cache_, key)` which uses seqlock CAS to:
1. claim the L2 entry's `seq` even→odd
2. set the entry's `stale` flag to 1
3. atomically `bucket_epoch.fetch_add(1)` on the bucket's epoch counter
4. release `seq+1` to even

**Why**: any TLS L1 reader on this host with `observed_epoch < new_epoch` will detect stale on their next access. The L2 entry's `stale=1` makes `cache_pool_lookup` return MISS. **This is the actual peer-visible state change** — host 0's worker can no longer hit cached K.

**Cost basis**: pure DRAM ops — open-addressed bucket scan (small) + seqlock CAS + atomic fetch_add + 1-byte store. ~30 ns measured.

**Source**: [src/cxl_kv_ops_A.cc:1342](src/cxl_kv_ops_A.cc#L1342) (probe I5 emitted right after); `cache_pool_set_stale` lives in [src/cxl_cache_pool.cc](src/cxl_cache_pool.cc).

## A12 — write `InvalRing.resp_op_id` ACK (probe I6)  [0.07 µs est]

**What**: InvalReceiver stores `e->status = 0` (success); RELEASE fence; `e->resp_op_id.store(op_id, RELEASE)` on cacheline 2; flush cacheline 2 (not line 1!); sfence.

**Why**: this is the ACK that A9 was waiting for. The cacheline split (req on line 1, resp on line 2 — iter-6A fix) means producer and consumer don't ping-pong on the same cacheline.

**Cost basis**: 1 ST-CXL on line 2 + 1 flush + 1 sfence ≈ 80 ns. Measured 70 ns.

**Source**: [src/cxl_kv_ops_A.cc:1345-1349](src/cxl_kv_ops_A.cc#L1345).

After this A12 ACK reaches host 1, WriteReceiver's A9 spin breaks, the broadcast loop continues (potentially more peers to invalidate at H > 2), and then W6 fires.

## A13 — W7 pool_alloc + W8 pool_write + ★ W9 publish ★ + W10 dir + cache_pool_insert + W12 return  [10 µs combined]

**What**: now that all peer caches are stale, WriteReceiver proceeds with the actual commit:
- **W7** `pool->alloc()` — CXL atomic `cursors_[host_1].bump.fetch_add(1)` returns a fresh `blk_off`. ~110 ns.
- **W8** `pool->write(blk_off, [value_len|value_bytes], 4+1024)` — memcpy 1028 B into per-host CXL pool segment at offset `blk_off`, per-cacheline `clflushopt`, final `sfence`. ~0.3 µs for KV=1024 (16 cachelines, but flushes are fire-and-forget).
- **★ W9 `publish_slot_cow` ★** — encode `(blk_off, size_class, fp)` into 8-B word, then atomically: write `slot->value`, flush, sfence; write `slot->key = key`, flush, sfence. This is the §I10 COMMIT POINT — when this sfence returns, peers reading the bucket will see (new_key, new_value) atomically.
- **W10** — `de->version++; de->state = SHARED; de->sharer_bitmap = 1 << host_1` (RESET to self only — peers must re-register via R3 if they want to re-cache); then `cache_pool_insert(K, V, value_len)` on host 1's L2 (seqlock CAS even→odd, write fields, bucket_epoch++, release even+1); then `tls_insert` if this thread has a TLS L1. **W10 mean 6.22 µs measured** — dominant because cache_pool_insert under T=64 MESI ping-pong on the shared L2 entry.
- **W12** — release `SlotDirectoryEntry.spinlock`; cleanup; return 0 to write_handler caller. ~1.5 µs.

**Why**: order matters — W9 commit happens AFTER all peer ACKs (W6) which is AFTER A12 ACK from InvalReceiver. This ordering is what enforces strict-A linearizability: peer reading new key/value can never have valid cached old value.

**Cost basis**: W7+W8 ~0.3 µs / W9 0.05 µs / W10 6.2 µs / W12 1.5 µs = ~8 µs measured. For KV=1024 the W8 memcpy is bigger so total ~10 µs.

**Source**: [src/cxl_kv_ops_A.cc:251-341](src/cxl_kv_ops_A.cc#L251).

## A14 — write `WriteRing.resp_op_id` ACK; worker A4 returns 0  [0.2 µs est]

**What**: back in write_receiver_loop (caller of write_handler), the loop does `e->status = result_status; ATOMIC_THREAD_FENCE(RELEASE); e->resp_op_id.store(op_id, RELEASE); flush_line(&e->resp_op_id); sfence; head++`. Worker on host 0 sees `resp_op_id == op_id` in its A4 spin loop, reads status, sets `req_op_id = 0` to free the slot, returns the status code.

**Why**: this completes the cross-host write protocol. Worker now knows write committed successfully.

**Cost basis**: WriteReceiver ACK ~70 ns (1 ST-CXL + flush + sfence) + worker observes (1 flush + LD-CXL on resp cacheline) + worker frees slot (1 ST-CXL + flush + sfence) ≈ 200 ns total.

**Source**: WriteReceiver ACK at [src/cxl_kv_ops_A.cc:1644-1648](src/cxl_kv_ops_A.cc#L1644); worker free-slot at [src/cxl_kv_ops_A.cc:90-93](src/cxl_kv_ops_A.cc#L90).

---

# Slide 3 — Protocol C Write Path

**Operation**: any host's worker calls `update(K, V=1024 B)`. No sharding — all hosts can write any key. Cross-host coordination is via the CXL-resident LFM mutex.

## C0 — `blockpool->alloc + blockpool->write(1024 B)`  [2.4 µs est]

**What**: for `value_len > 8 B`, the variadic `update(K, void*, len)` first allocates a per-host CXL pool block (`bump.fetch_add` atomic returns `new_off`), then memcpy + flush + sfence the 1024 value bytes into the pool. Then calls `update(K, new_off)` with the 8-B offset as the slot value.

**Why**: per [C variadic-length API](src/cxl_kv_ops_C.cc#L879), inline u64 fast path is used for `len ≤ 8`; pool path for larger. Slot still holds 8 B (the blk_off); the bytes live in the pool.

**Cost basis**: 1× CXL atomic fetch_add (~1.4 µs) + 16 cacheline flush+sfence (~1 µs).

**Source**: [src/cxl_kv_ops_C.cc:879-897](src/cxl_kv_ops_C.cc#L879).

## C1 — `lock_table_.lock(idx)` (LFM acquire, cross-host blocking)  [25.0 µs measured avg, T=64]

**What**: worker calls `lock_table_.lock(bucket_idx, host_id, num_hosts)`. This invokes `shm_mutex_lock(&entries_[idx].mutex)` — an LFM (Lamport's Fast Mutex) lock implementation from the sibling `cxl_shm_profiling` repo. LFM is a 5-step rendezvous protocol that uses 2 CXL cacheline atomic stores per host (per protocol step `lfm_localstore` ~16 ns, `lfm_peerscan` ~2.8 µs uncontested) plus a fallback peer-priority slow path.

**Why**: ALL hosts can write any bucket → need cross-host serialization. LFM is preferred over a naive spinlock because it's bounded-wait per Lamport's algorithm (or ticket lock when `FUSEE_USE_TICKET_LOCK=1`).

**Cost basis**: uncontested ~4.4 µs (per `latency_decomp_C iter-3` LFM anatomy measurement). At T=64 cross-host hot bucket: **mean 25 µs / p99 427 µs** — this is the dominant cost of C's write path (84% of total), the central scalability bottleneck.

**Source**: [src/cxl_kv_ops_C.cc:429 (lock call)](src/cxl_kv_ops_C.cc#L429); LFM implementation in `~/cxl_shm_profiling/locks/lfm_lock.c`.

## C2 — `flush_line(slots[0]); flush_line(slots[4]); mfence`  [0.15 µs measured]

**What**: bucket is 7 × 16 B = 112 B = 2 cachelines. flush both cachelines + 1 mfence. Phase-2.6 "flush-collapse" optimization: 2 flushes cover all 7 slots' key+value pairs.

**Why**: under the bucket lock, we have exclusive access to the bucket — but the LOCAL CPU might have a stale cached copy from before another host wrote it. Flushing forces re-fetch from CXL.

**Cost basis**: 2 × clflushopt (66 ns) + 1 mfence (23 ns) ≈ 155 ns. Measured 150 ns (counted as part of "Scan" decomp stage which totals 1.40 µs).

**Source**: [src/cxl_kv_ops_C.cc:311-313](src/cxl_kv_ops_C.cc#L311) (in `update`).

## C3 — scan 7 slots; pick `target_slot`  [1.25 µs measured]

**What**: linear loop `for s in 0..6: if slots[s].key == key: target = s; break`. After C2's flush, each `slots[s].key` LD pulls a cacheline from CXL on first miss (but Phase-2.6 collapsed flushes mean the 2 cachelines are now hot in L1).

**Why**: find which slot holds K (UPDATE/DELETE) or first empty slot (INSERT). Under bucket lock, this scan is race-free.

**Cost basis**: should be CPU-bound on hot cachelines (< 50 ns). But measured 1.25 µs (rest of the 1.40 µs "Scan" budget) suggests some MESI re-fetching is happening at T=64 — possibly because between flush and scan, a peer host concurrently wrote a different slot in this bucket and the cacheline got re-invalidated.

**Source**: [src/cxl_kv_ops_C.cc:314-318](src/cxl_kv_ops_C.cc#L314).

## C4 — `slot.value = blk_off; flush; sfence`  [0.02 µs measured]

**What**: write the 8-B `value` field of the target slot (= `blk_off` for KV>8), then `clflushopt(&slot->value)` + `sfence`.

**Why**: this writes the new value visible to all hosts after the sfence. But wait — alone this is NOT yet the linearization point! A peer reader might not see the change until our `bucket_epoch` bump (C5) triggers their seqlock retry. UPDATE only changes `value`, not `key`, so a racing reader scanning the bucket either sees old_value (if their flush happened before ours) or new_value — both committed values.

**Cost basis**: 1 ST + 1 clflushopt + 1 sfence ≈ 80 ns. Measured 18 ns (because flush is fire-and-forget and the slot cacheline was already exclusive in this core's cache).

**Source**: [src/cxl_kv_ops_C.cc:347-349](src/cxl_kv_ops_C.cc#L347).

## C5 — ★ `__atomic_add_fetch(&write_epoch, 1) + flush + sfence` ★  [4.07 µs measured]

**What**: atomically increment the bucket's `write_epoch` counter (CXL atomic RMW), then `clflushopt` the cacheline and `sfence`. **This is the linearization point** — when this sfence returns, the new epoch is globally visible.

**Why**: C readers seqlock on `write_epoch`. Once they observe `cxl_epoch != cached_epoch`, they know to flush+re-scan the bucket and invalidate their DRAM cache copy. Bumping epoch AFTER C4 ensures any reader who sees the new epoch will also (on re-scan) see the new slot value.

**Cost basis**: CXL atomic add_fetch + flush + sfence = `cxl_primitive_bench` measured **1.435 µs p50, 1.7 µs p99**. Measured 4.07 µs is higher because at T=64 the write_epoch cacheline is contended across hosts (many writers on the same bucket all atomically incrementing).

**Source**: [src/cxl_kv_ops_C.cc:119-123](src/cxl_kv_ops_C.cc#L119) (`bump_epoch`), called at [353](src/cxl_kv_ops_C.cc#L353).

**Note iter-2 ordering trick**: `unlock` (C6) is called BEFORE C5 in the per-slot-lock variant — slot lock only serializes same-slot writers; the bucket epoch is the reader's seqlock tag and is itself atomic. Letting the next same-slot writer enter while we bump epoch saves ~2 µs from the hot-slot critical section. See [src/cxl_kv_ops_C.cc:351-353 comment](src/cxl_kv_ops_C.cc#L351).

## C6 — `lock_table_.unlock()` (LFM release) → return 0  [0.03 µs measured]

**What**: `shm_mutex_unlock(&entries_[idx].mutex)` — LFM release writes its internal state cacheline, flush, sfence. Returns 0 to caller.

**Why**: free the bucket lock so the next writer (possibly on a different host) can proceed.

**Cost basis**: ~30 ns measured (LFM release is asymmetrically cheaper than acquire because no rendezvous required — just store a 0).

**Source**: [src/cxl_kv_ops_C.cc:468](src/cxl_kv_ops_C.cc#L468) (uncontested per-bucket variant) / [352 in iter-2 per-slot variant](src/cxl_kv_ops_C.cc#L352).

---

# Slide 4 — Protocol A Read Path

**Operation**: host 0 worker calls `search(K)`. Owner of K is host 1. We show the **cross-host miss path** (most expensive case, ~0.2% of reads at T=64 workload-a but determines tail latency).

## A1r — enter search(K); compute bucket_idx  [0.05 µs est]

**What**: `if (key == kEmptyKey) return -1; PROBE_OP("R1", key);` — emit R1 probe; bucket_idx computed on first L2 path (not needed here yet).

**Why**: pure entry boilerplate.

**Cost basis**: 1 FNV-1a hash + 1 branch ~15 ns. R1 probe takes the timestamp.

**Source**: [src/cxl_kv_ops_A.cc:1696-1697](src/cxl_kv_ops_A.cc#L1696).

## A2r — TLS L1 lookup: `bucket_epoch.load` + `tls_lookup` (★ on hit: ~80 ns return)  [0.05 µs est]

**What**: if `g_thread_tls` is set (worker called `set_thread_tls_cache(tls)` post-fork), load `cache_pool_bucket_epoch(cache_, key)` (1 LD on shared L2's bucket epoch cacheline), then `tls_lookup(g_thread_tls, key, cur_epoch, ...)` — linear probe in per-worker private DRAM, up to 8 slots. On hit: epoch match → return value immediately with PROBE_OP("R0_tls_hit"). On miss/stale-epoch: evict TLS slot, fall through.

**Why**: TLS L1 is the **fastest read path** — private DRAM, 0 cross-core MESI traffic, just one cross-bucket epoch atomic load. Most ops at workload-a/b/c hit TLS (18-80% depending on workload+KV).

**Cost basis**: 1× LD on shared cacheline (cross-core MESI, ~50 ns) + linear probe in L1 cache (~20 ns) + value memcpy from L1 (~5 ns) = ~80 ns on hit. PROBE_OP("R0_tls_hit") fires only on hit.

**Source**: [src/cxl_kv_ops_A.cc:1704-1717](src/cxl_kv_ops_A.cc#L1704).

## A3r — `cache_pool_lookup` L2 seqlock CAS read (★ on hit: ~7 µs return)  [0.5 µs measured]

**What**: shared L2 cache_pool entry seqlock pattern. Loop up to 8 times:
1. `seq0 = entry->seq.load()` — even = stable, odd = mid-update
2. if odd: pause, retry
3. load `key`, `stale`, `value_size`; memcpy `value_bytes`
4. `seq1 = entry->seq.load()`; if `seq0 == seq1 && key matches && !stale` → HIT
5. else retry

**Why**: L2 is `MAP_SHARED` across same-host workers (and cross-host writes can mark entries stale via InvalReceiver). Seqlock allows writers to commit without blocking readers; readers detect mid-update via odd seq.

**Cost basis**: ~150 ns hit. But the `value_bytes` memcpy is the dominant cost for KV=1024 (16 cachelines) — measured **7 µs at T=64** because under hot Zipf, the L2 entry's value_bytes is in MESI ping-pong across cores.

**Source**: [src/cxl_kv_ops_A.cc:1720-1734](src/cxl_kv_ops_A.cc#L1720); `cache_pool_lookup` in `src/cxl_cache_pool.cc`.

On hit: `cache_pool_insert(L1)` to populate TLS, PROBE_OP("R6"), return. On miss: PROBE_OP("R2miss"), fall through.

## A4r — L2 miss → `my_epoch_at_send = bucket_epoch.load(K)` (C13 tag)  [0.1 µs est]

**What**: load `cache_pool_bucket_epoch(cache_, key)` AGAIN — but THIS one is the "C13 tag" we record before sending the request. It captures "the freshest L2 view this reader has observed of bucket K at the moment of send."

**Why**: per iter-11A C13 invariant — receiver's `lookup_epoch` (recorded when their `read_handler` reads the bucket) must be `>= my_epoch_at_send`. If receiver's view was OLDER, then an invalidate we should have applied first may not yet be visible to receiver → reader rejects with `-3` to avoid caching a stale value.

**Cost basis**: 1× LD on shared cacheline ~50-100 ns.

**Source**: [src/cxl_kv_ops_A.cc:1400-1401](src/cxl_kv_ops_A.cc#L1400) (inside `forward_read_direct`).

## A5r — `staging.ready_op_id = 0`; flush; sfence  [0.1 µs est]

**What**: before sending the request, explicitly clear the staging slot's `ready_op_id` so we don't observe a leftover signal from a PREVIOUS user of this slot (slots are reused in ring-buffer fashion).

**Why**: ReadStaging slots are recycled per `slot_idx`. If we don't clear, our A8r spin could falsely match a previous owner's `ready_op_id`.

**Cost basis**: 1 ST-CXL + flush + sfence ≈ 80 ns.

**Source**: [src/cxl_kv_ops_A.cc:1405-1409](src/cxl_kv_ops_A.cc#L1405).

## A6r — `ReadRing[0][1].tail.fetch_add(1)` (CXL atomic)  [1.4 µs est]

**What**: claim ReadRing slot via atomic fetch_add on tail, flush+sfence. Same primitive as A2 (write path's WriteRing.tail).

**Why**: ReadRing is a separate SPSC ring from WriteRing (op 4 CACHE_REGISTER only). Tail RMW serializes same-host workers' request claims.

**Cost basis**: CXL `fetch_add + flush + sfence` ~1.4 µs.

**Source**: [src/cxl_kv_ops_A.cc:1386-1389](src/cxl_kv_ops_A.cc#L1386).

## A7r — write `ReadEntry{key, req_op_id}`; flush; sfence  [0.1 µs est]

**What**: wait-for-slot-free (`req_op_id == 0`), then fill `e->key = key; e->resp_op_id = 0; e->status = 0; e->resp_value_len = 0; e->resp_blk_off = 0;` RELEASE fence; `e->req_op_id.store(op_id, RELEASE)`; flush + sfence.

**Why**: the standard "publish request" pattern same as A3 on the write path.

**Cost basis**: ~80 ns (mostly the flush+sfence on req cacheline).

**Source**: [src/cxl_kv_ops_A.cc:1392-1419](src/cxl_kv_ops_A.cc#L1392).

## A8r — worker spin on `staging.ready_op_id` (200 ms cap)  [~14 µs blocked, measured R3]

**What**: worker enters poll loop on the **staging slot's `ready_op_id`** (NOT the ring's `resp_op_id` — that's the iter-11A Phase 1 innovation, fuses ACK + value delivery). Loop: flush staging.ready_op_id, full_fence, load, compare. If `== op_id` → break. If timeout 200 ms → return `-2` and free the ring slot.

**Why**: iter-11A Phase 1 forwarder-pool-direct — instead of waiting for ring ACK then doing a separate pool->read, we wait directly on the staging slot which will carry the value bytes too. Saves one CXL roundtrip (the pool->read after ack).

**Cost basis**: wall time of A9r..A14r ≈ 14 µs measured (PROBE_OP("R3") and PROBE_OP("R4") bracket this interval).

**Source**: [src/cxl_kv_ops_A.cc:1422-1446](src/cxl_kv_ops_A.cc#L1422).

## A9r — ReadReceiver poll + reads ReadEntry  [1.5 µs est]

**What**: host 1's ReadReceiver thread (cpu 67) is in its main loop polling all ReadRing[*][1].tail values. Sees host 0's tail moved → reads ReadEntry's req_op_id, key.

**Why**: ReadReceiver is the only consumer of ReadRing[*][1].

**Cost basis**: same as A5 — 1× flush + LD on tail + 1× flush + LD on entry ~1.5 µs.

**Source**: [src/cxl_kv_ops_A.cc:1658-1692](src/cxl_kv_ops_A.cc#L1658) (`read_receiver_loop`).

## A10r — flush bucket + scan + acquire `SlotDirectoryEntry.spinlock`; `sharer_bitmap |= (1 << src)` (★ peer-visible state change)  [5 µs est]

**What**: in [`read_handler`](src/cxl_kv_ops_A.cc#L1522):
1. flush both bucket cachelines + mfence
2. scan 7 slots looking for matching key
3. on found: acquire `SlotDirectoryEntry.spinlock` (host-local DRAM); set `de->sharer_bitmap |= (1 << src)` to register requesting host as sharer (so future writers will invalidate it)
4. read slot.value (the encoded `(blk_off, size_class, fp)`)
5. release spinlock

**Why**: §I9 requires that before a reader caches a value, the owner directory records them as sharer. Otherwise the next writer's invalidate broadcast would miss this reader → strict-A violated. This is the AP15 "register-then-fill" protocol.

**Cost basis**: 2× flush + mfence + 7-slot scan (~200 ns) + spinlock acquire (~50 ns DRAM) + 1-byte OR + LD encoded value + spinlock release. Estimate 5 µs because hot bucket cachelines under T=64 may need cross-host re-fetch.

**Source**: [src/cxl_kv_ops_A.cc:1524-1577](src/cxl_kv_ops_A.cc#L1524).

## A11r — `pool->read(blk_off, 4) → value_len; pool->read(blk_off+4, 1024)`  [5 µs est]

**What**: from the encoded slot.value, decode `blk_off`. Two pool reads:
1. `pool->read(blk_off, hdr, 4)` reads the 4-B value_len header
2. `pool->read(blk_off + 4, staging.value_bytes, value_len)` reads the value bytes DIRECTLY into the staging arena

**Why**: pool block layout = 4-B header + value bytes. Iter-11A Phase 1's direct-deposit puts the value into staging here (NOT a separate buffer on ReadReceiver's stack) so the reader can just memcpy from staging.

**Cost basis**: pool reads use per-cacheline flush + LD-CXL on pool segment. For KV=1024 = 16 cachelines × ~600 ns each (with prefetch amortization) ≈ 5 µs.

**Source**: [src/cxl_kv_ops_A.cc:1599-1612](src/cxl_kv_ops_A.cc#L1599).

## A12r — write `staging.value_bytes + lookup_epoch + status`; flush 16 CL  [1.0 µs est]

**What**: at this point staging.value_bytes already has the value (from A11r's direct read). ReadReceiver now stores the control fields on cacheline 0 of staging:
- `st->key = key`
- `st->value_size = vlen`
- `st->status = 0`
- `st->lookup_epoch = cache_pool_bucket_epoch(cache_, key)` — the C13 tag from owner's perspective
- flush control cacheline
- flush each value cacheline (16× for 1024 B)
- sfence

**Why**: lookup_epoch is the owner's view of bucket K at handler-dispatch time. The reader compares against `my_epoch_at_send`; if `lookup_epoch < my_epoch_at_send` → owner's view was older → reject.

**Cost basis**: 17 clflushopt + sfence ~1 µs.

**Source**: [src/cxl_kv_ops_A.cc:1537-1554](src/cxl_kv_ops_A.cc#L1537) (`publish_staging` lambda).

## A13r — ★ `staging.ready_op_id = req_op_id` (release-publish) ★  [0.07 µs est]

**What**: AFTER all the staging fields are flushed, atomically store `ready_op_id = req_op_id` with RELEASE memory order, then flush ready_op_id cacheline + sfence.

**Why**: this is the release-publish point. Reader (A8r spin) acquires on this same cacheline. After this store + flush, reader can observe `ready_op_id == req_op_id` and is guaranteed to see all the staging fields populated.

**Cost basis**: 1 ST-CXL + flush + sfence ~80 ns.

**Source**: [src/cxl_kv_ops_A.cc:1552-1554](src/cxl_kv_ops_A.cc#L1552).

## A14r — worker A8r spin observes `ready_op_id == req_op_id` → break  [< 0.1 µs est]

**What**: worker's spin loop on host 0 sees the ACQUIRE load returns `req_op_id`. Breaks out of the spin loop. Frees the ring slot by clearing req_op_id (CRITICAL — even if A8r timed out, the slot must be freed or the ring deadlocks at >256 reads per (req, owner) pair, per iter-11A 2026-05-10 Phase 1 bug fix).

**Why**: matches the release-publish from A13r. The flush+LD on ready_op_id is the cross-host signal travel.

**Cost basis**: 1 flush + LD-CXL on ready cacheline ~70 ns + 1 ST-CXL + flush + sfence on req cacheline to free slot ~80 ns ≈ 150 ns.

**Source**: [src/cxl_kv_ops_A.cc:1426-1446](src/cxl_kv_ops_A.cc#L1426).

## A15r — C13 validate; memcpy from staging  [0.4 µs measured R4]

**What**: after PROBE_OP("R4"), validate `staging.lookup_epoch >= my_epoch_at_send`. If less → return `-3` (stale snapshot, caller retries from L2). If status != 0 (e.g. key-not-found) → return that. Otherwise, flush each value cacheline (16× for KV=1024) and memcpy from staging into the caller's buffer.

**Why**: C13 invariant guards against caching a stale value when an invalidate concurrent with the read might not have been seen by the receiver.

**Cost basis**: 16 clflushopt + 1 mfence + memcpy 1024 B ≈ 1 µs theory. Measured **R4 = 0.39 µs** because clflushopts are fire-and-forget and memcpy is L1-bound after flushes.

**Source**: [src/cxl_kv_ops_A.cc:1452-1486](src/cxl_kv_ops_A.cc#L1452).

## A16r — `cache_pool_insert` (L2) + `tls_insert` (L1) + return  [1.1 µs measured R6+misc]

**What**: after AP15 (cache populate only after register ACK), insert the just-fetched value into both cache layers:
1. `cache_pool_insert(cache_, K, value, value_len)` — seqlock CAS even→odd, write fields, bump bucket_epoch, release even+1
2. if `g_thread_tls` set: `tls_insert(g_thread_tls, K, value, value_len, cur_epoch)` — write to private DRAM
3. PROBE_OP("R6"); return 0

**Why**: §AP15 — cache populate ONLY after register ACK so any concurrent writer's invalidate (which would mark our cache stale) has either already arrived OR is guaranteed to arrive on the next R3 → C13 retry cycle.

**Cost basis**: cache_pool_insert seqlock CAS + 1024-B memcpy ~1 µs + tls_insert <100 ns.

**Source**: [src/cxl_kv_ops_A.cc:1747-1762](src/cxl_kv_ops_A.cc#L1747).

---

# Slide 5 — Protocol C Read Path

**Operation**: any host's worker calls `search(K)`. No cross-host coordination ever required for reads — the seqlock on `write_epoch` is enough for LRC semantics.

## C1r — `bucket_idx = fnv1a_u64(K) % B; entry = lock_table_.entry(idx)`  [0.015 µs est]

**What**: pure CPU work — hash K to bucket index, get pointer to that bucket's BucketLockEntry (which holds the LFM mutex + write_epoch).

**Why**: entry boilerplate. `lock_table_.entry()` is just `entries_ + idx` pointer arithmetic.

**Cost basis**: 1 FNV-1a + 2 pointer ops ~15 ns.

**Source**: [src/cxl_kv_ops_C.cc:561-563](src/cxl_kv_ops_C.cc#L561).

## C2r — `CACHELINE_LOAD(&write_epoch)` (★ cache hit early exit)  [0.7 µs est]

**What**: if DRAM cache is enabled (`enable_dram_cache(true)`):
1. read `cache_epoch_[idx]` (per-process DRAM, fast)
2. if `cached != UINT64_MAX`, load `cxl_epoch = CACHELINE_LOAD(&entry->write_epoch)` (one CXL cacheline LD)
3. if `cxl_epoch == cached`: bucket hasn't been written since our last cache fill → scan the DRAM copy in `cache_buckets_[idx]` without any CXL flush
4. if matching key found in DRAM scan → return value
5. else fall through to C3r

**Why**: LRC says "returned value was committed at some instant during scan". If no write has happened to this bucket since we cached it, every value in the cached bucket is still valid → scan DRAM without flushing CXL.

**Cost basis**: 1× LD-CXL on write_epoch cacheline (with the flush_line + mfence that CACHELINE_LOAD does) ≈ 700 ns. The subsequent DRAM scan is negligible.

**Source**: [src/cxl_kv_ops_C.cc:566-582](src/cxl_kv_ops_C.cc#L566).

## C3r — cache miss → `flush_line(slots[0]) + flush_line(slots[4]) + mfence`  [0.15 µs est]

**What**: same Phase-2.6 flush-collapse as the writer's C2 — 2 flushes covering the 7-slot bucket + 1 mfence.

**Why**: epoch mismatch or cache disabled → must re-fetch bucket cachelines from CXL to see the current contents.

**Cost basis**: 2 × 66 ns + 23 ns mfence ≈ 155 ns.

**Source**: [src/cxl_kv_ops_C.cc:596-598](src/cxl_kv_ops_C.cc#L596).

**Note Phase-2.4 read-singleshot**: BEFORE this scan, we already loaded `e1 = CACHELINE_LOAD(&write_epoch)` (call it the "scan-snapshot"). After the scan we do NOT re-validate the epoch — LRC allows the returned value to be "some valid instant" during the scan. If we wanted strict-A here we would re-load e1' and compare, retrying on mismatch. That's exactly what Protocol A's L2 seqlock CAS reader does (see slide 4 A3r).

## C4r — scan 7 slots in L1; get `slot.value = blk_off` (★ LRC: no revalidation)  [0.05 µs est]

**What**: linear loop `for s in 0..6: if slots[s].key == key: captured = slots[s].value; break`. If DRAM cache enabled: also copy the entire bucket into `cache_buckets_[idx]` AND set `cache_epoch_[idx] = e1` (the pre-scan epoch snapshot).

**Why**: x86 8-B aligned load is atomic per slot field. Even if a concurrent writer is modifying a different slot, our slot read returns either `(old_key, old_value)`, `(new_key, new_value)`, or one of the two transient `(old_key, new_value)` / `(new_key, old_value)` mixed states. The break on `key == K` means we only ever return `(key=K, value=committed_value_for_K_at_some_instant)`.

**Cost basis**: up to 7 LDs on L1 (cachelines hot from C3r flush) ~50 ns.

**Source**: [src/cxl_kv_ops_C.cc:602-609](src/cxl_kv_ops_C.cc#L602).

## C5r — `blockpool->read(blk_off, 1024)` (16 CL × LD-CXL for value bytes)  [1.0 µs est]

**What**: for KV>8 variadic path, slot.value is a `blk_off` into the per-host blockpool. Need to fetch the actual 1024 value bytes: `blockpool_->read(v, out_buf, value_len)` which does per-cacheline flush + LD-CXL.

**Why**: blockpool stores the value bytes; slot only stores the offset.

**Cost basis**: 16 cachelines × LD-CXL with flush ~1 µs (prefetch amortizes).

**Source**: [src/cxl_kv_ops_C.cc:914-918](src/cxl_kv_ops_C.cc#L914) (in variadic search wrapper).

---

# Summary of correspondence

| Step (slide 2-5) | Single-stage measured probe(s) | Source func | Linearization role |
|---|---|---|---|
| A1 | (untracked) | `forward_write_direct` staging memcpy | |
| A2 | (untracked) | `forward_write_direct` tail.fetch_add | |
| A3 | (untracked) | `forward_write_direct` entry publish | |
| A4 | (untracked, spin) | `generic_spin_wait` | |
| A5 | (untracked) | `write_receiver_loop` poll | |
| A6 | W1+W2+W3 | `execute_write_local` prefix | |
| A7 | I1 | `send_invalidate_direct` fetch_add | |
| A8 | I2 | `send_invalidate_direct` write entry | |
| A9 | (untracked, spin) | `send_invalidate_direct` spin | |
| A10 | I3+I4 | `inval_receiver_loop` poll+read | |
| A11 | I5 | `cache_pool_set_stale` | **#1 cache-stale on peer** |
| A12 | I6 | `inval_receiver_loop` ACK | |
| A13 | W7+W8+W9+W10+W12 | `execute_write_local` suffix | **#2 W9 slot CoW + #3 W10 bitmap reset** |
| A14 | (untracked) | `write_receiver_loop` ACK | |
| C0 | (untracked, pre-op) | `update(K, void*, len)` variadic | |
| C1 | Lock | `lock_table_.lock` LFM | |
| C2 | Scan-flush | flush_line × 2 + mfence | |
| C3 | Scan-loop | slot scan | |
| C4 | Publish | slot.value store + flush | (slot publish) |
| C5 | Epoch | `bump_epoch` atomic + flush | **★ peer-visible commit** |
| C6 | Unlock | `lock_table_.unlock` | |
| A1r | R1 | `search()` entry | |
| A2r | R0_tls_hit (on hit) | `tls_lookup` | TLS epoch check |
| A3r | R2hit/R2miss | `cache_pool_lookup` seqlock | L2 seqlock check |
| A4r | (untracked) | C13 epoch capture | |
| A5r-A7r | (untracked) | ReadRing dispatch | |
| A8r | R3 (timestamp at start of spin) | spin on staging | |
| A9r-A14r | (untracked, span between R3-R4) | `read_handler` + staging publish | A10r sharer_bitmap |= (peer state) |
| A15r | R4 | C13 validate + memcpy | C13 epoch check |
| A16r | R6 | cache populate + return | AP15 cache populate |
| C1r | (no probe) | hash | |
| C2r | (no probe) | epoch load + DRAM scan | epoch validity check |
| C3r | (no probe) | bucket flush | |
| C4r | (no probe) | bucket scan | LRC: no revalidation |
| C5r | (no probe) | blockpool->read value bytes | |

---

# Quick reference: which stages are dominant cost

**Protocol A write** (T=64 KV=1024 with peer cached, end-to-end ≈ 22 µs):
- A4 worker spin (~22 µs blocked, but that's just wait time for A5-A14)
- Inside A4: **A6 = 5 µs (W1 bucket flush dominates)** + **A13 = 10 µs (W10 cache_pool_insert MESI ping-pong dominates)** + invalidate sub-loop A7-A12 ≈ 5 µs
- **W10 alone = 6.22 µs measured = the single dominant per-op cost**

**Protocol C write** (T=64 KV=1024, end-to-end ≈ 33 µs):
- **C1 LFM lock = 25 µs = 76% of total** — the single dominant cost is the cross-host LFM acquire under T=64 contention

**Protocol A read** (TLS hit 80 ns | L2 hit 7 µs | cross-host miss 22 µs):
- TLS hit: 1× epoch LD ~80 ns
- L2 hit: 1× epoch LD + 16-CL value_bytes memcpy under MESI (~7 µs)
- Cross-host miss: A6r ring fetch_add (1.4 µs) + A8r spin (14 µs) + A11r pool->read (5 µs) — **R3 = 14 µs measured = dominant**

**Protocol C read** (cache hit 1.7 µs | cache miss 1.2 µs):
- 1× write_epoch LD-CXL + DRAM/L1 scan + (if KV>8) blockpool->read 16 CL
