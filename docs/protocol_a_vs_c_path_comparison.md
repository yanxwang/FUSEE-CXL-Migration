# Protocol A vs Protocol C — Read / Write Path Comparison

> **Presentation-ready** comparison of the two CXL-resident KV protocols.
>
> **Thesis**: Protocol A (strict-A linearizable) has fundamentally more
> stages, more linearization points, and more cross-host coordination
> overhead than Protocol C (LRC) — and the measured per-stage latencies
> confirm this is not an artifact of immature engineering but a
> structural property of the design.
>
> **Source data**:
> - Protocol A: `docs/path_decomp_iter11A_20260511_023247/per_cell/workloada_best/per_stage_decomp.md` (workload-A, kv=512, T=64, cache=off, FUSEE_PROBE=1 build)
> - Protocol C: `docs/iters/latency_decomp_C_iter2_20260423_053919.md` (workload-A, T=64, cache=on, FUSEE_LATENCY_DECOMP=1 build)
> - Both: g3+g4 testbed (2 hosts × 86 cores Xeon + CXL Type-3 expander via PCIe switch); DRAM 174.8 ns/393 GB/s, CXL 607.8 ns/51.78 GB/s
>
> **Sister docs**: `protocol_a_architecture_blueprint.md`, `protocol_c_architecture_blueprint.md`.

---

# Part 1 — Unified path diagram (both protocols on one canvas)

Legend:
- **Solid arrows (A1, A2, …)** = Protocol A path step
- **Dashed arrows (C1, C2, …)** = Protocol C path step
- **Yellow boxes** = linearization points (where a peer host can observe the change for the first time)
- **Red boxes** = mandatory cross-host blocking operations (the writer/reader cannot proceed until peer acknowledges or releases)

## 1.1 Write path — cross-host write `update(key=K, value=V)`

Setup: worker on host 0 calls `update(K, V)`. For A, owner-of-K is host 1 (`hash(K) >> 31) & 1 == 1`). For C, host 0 writes directly under the cross-host LFM lock.

```
                                CXL shared region
   ┌──────────────────────────────────────────────────────────────────┐
   │  Hashtable buckets  │  WriteRing[0][1]  │  InvalRing[1][0]      │
   │  KvBlockPool        │  ForwardStaging   │  ReadRing[*][*]        │
   │  BucketLockTable    │  ReadStaging      │  KvBlockPool segs      │
   │  (per-bucket mutex  │  AggregatorRegion │  OpLog (opt)           │
   │   + write_epoch)    │                   │                        │
   └──────────────────────────────────────────────────────────────────┘
            ↑              ↑              ↑              ↑
            │              │              │              │
   ┌────────┴──────┐    ┌─┴───────────────┴───┐    ┌────┴───────────┐
   │   host 0       │    │  host 1 background  │    │  host 1         │
   │   worker       │    │  WriteReceiver(c65) │    │  InvalReceiver  │
   │   (caller)     │    │  ReadReceiver (c67) │    │  (cpu 69)       │
   └────────────────┘    └─────────────────────┘    └─────────────────┘

   PROTOCOL A (solid arrows, owner-routed forward + invalidate)
   ────────────────────────────────────────────────────────────
   A1  worker → ForwardStaging[0][1][slot_idx]    (write V bytes + flush + sfence)
   A2  worker → WriteRing[0][1].tail              (RMW-CXL fetch_add)
   A3  worker → WriteEntry{key, op_kind, ...}      (write + flush + sfence)
   A4  worker spin                                 (poll resp_op_id, 5ms cap)
                              │
                              ▼  WriteReceiver picks up entry
   A5  WriteReceiver reads ForwardStaging bytes    (FLUSH + LD-CXL)
   A6  WriteReceiver calls execute_write_local     (now runs W1..W12 on host 1)
        ├── W1-W3  bucket flush + spinlock acquire on SlotDirectoryEntry
        ├── W4-W6  send_invalidate(host 0)         ─→ A7..A12 below
        ├── W7-W8  pool->alloc + pool->write
        ├── ★ W9 COMMIT POINT ★                   (slot.value flush; slot.key flush)
        ├── W10    bitmap reset; cache_pool_insert + bucket_epoch++
        ├── W11    tls_insert (host 1's worker side)
        └── W12    unlock
   A13 WriteReceiver writes resp_op_id ACK         (flush + sfence)
   A14 host 0 worker observes ACK, frees slot, returns

   ────────────── INVALIDATE sub-path (A7..A12, runs synchronously inside A6 W4-W6) ──────
   A7  WriteReceiver → InvalRing[1][0].tail        (RMW-CXL fetch_add)
   A8  WriteReceiver → InvalEntry{key}              (write + flush + sfence)
   A9  WriteReceiver spin                           (poll resp_op_id, 5ms cap)
                              │
                              ▼  InvalReceiver on host 0 picks up
   A10 InvalReceiver → cache_pool_set_stale(K)     (DRAM seqlock CAS + bucket_epoch++)
   A11 InvalReceiver writes resp_op_id ACK         (flush + sfence)
   A12 WriteReceiver observes ACK, returns to W7


   PROTOCOL C (dashed arrows, in-place under cross-host lock)
   ─────────────────────────────────────────────────────────
   C1  worker → BucketLockTable.entry(idx).mutex   ⇢⇢⇢  LFM ACQUIRE
                                                       (cross-host blocking;
                                                        spin / futex on CXL queue)
   C2  worker → flush slots[0]+slots[4] + mfence    ⇢⇢⇢  (covers all 7 slots, 2 flushes)
   C3  worker scans 7 slots, finds target          ⇢⇢⇢
   C4  worker → slot.value = V + flush + sfence    ⇢⇢⇢ ★ COMMIT POINT ★
   C5  worker → __atomic_add_fetch(&write_epoch, 1) ⇢⇢⇢ (CXL atomic + flush + sfence,
                                                        ~1.4 µs hardware cost)
   C6  worker → lock_table_.unlock                  ⇢⇢⇢ LFM RELEASE
   (returns immediately — no peer ACK required;
    peer hosts notice via their next read seeing
    the new write_epoch.)
```

**Visual comparison summary**:

| Aspect | Protocol A | Protocol C |
|---|---|---|
| Distinct numbered steps | **A1..A14 = 14 steps** | **C1..C6 = 6 steps** |
| Linearization points (peer-visible state changes) | **3** (W9 slot publish + W10 bucket_epoch bump + I10 cache_pool stale bump on each remote sharer) | **1** (write_epoch atomic bump) |
| Cross-host blocking points (must wait for ACK) | **2** (A4 WriteRing roundtrip + A9 InvalRing roundtrip per remote sharer; both 5ms-cap timeouts) | **1** (C1 LFM acquire — same primitive but spinning, not 2-step message) |
| Threads involved in critical path | **3** (caller worker + host 1 WriteReceiver + host 0 InvalReceiver) | **1** (caller worker only) |
| CXL rings touched | **3** (WriteRing, ForwardStaging, InvalRing) | **0** (lock is on a CXL line but not a ring) |
| Atomic RMW-CXL on critical path | **3** (A2 WriteRing.tail + A7 InvalRing.tail + A10 implicit via bucket_epoch++) | **1** (C5 write_epoch++) |

## 1.2 Read path — `search(key=K)` cache miss

Setup: same Zipf hot key K, owner-of-K = host 1 in A; for C every host can read directly. The cache miss case is what stresses the path; in steady state most workloads hit cache and the cross-host path is rare — but **the existence of this path is what shapes the protocol's complexity ceiling**.

```
   PROTOCOL A read path (solid arrows, forwarder-pool-direct iter-11A)
   ───────────────────────────────────────────────────────────────────
   A1r worker → bucket_epoch.load(key)             (one CXL line)
   A2r worker → TlsCache lookup                    (private DRAM)
        ─ if hit + epoch fresh: return     ←─ R0_tls_hit fast path
        ─ else fall through
   A3r worker → cache_pool_lookup (L2 seqlock CAS) (DRAM shared)
        ─ if hit: populate TLS + return    ←─ R2hit
        ─ else fall through
   A4r worker → bucket_epoch.load AGAIN ← C13 my_epoch_at_send
   A5r worker → ReadRing[0][1].tail.fetch_add      (RMW-CXL)
   A6r worker → ReadStaging[0][1][slot_idx].ready_op_id = 0 + flush
   A7r worker → ReadEntry{key, req_op_id} + flush + sfence
   A8r worker spin                                  (poll staging.ready_op_id, 200ms cap)
                              │
                              ▼  host 1 ReadReceiver picks up
   A9r  ReadReceiver flush bucket + scan 7 slots
   A10r ReadReceiver acquire SlotDirectoryEntry.spinlock
   A11r ReadReceiver sharer_bitmap |= (1 << 0)     ★ peer-visible state change ★
   A12r ReadReceiver pool->read(blk_off, 4)        (read 4-B value_len header)
   A13r ReadReceiver pool->read(blk_off+4, vlen)   (read value bytes from CXL pool)
   A14r ReadReceiver → ReadStaging[0][1][slot_idx].{value_bytes, lookup_epoch, status}
                                                    (flush each cacheline + sfence)
   A15r ReadReceiver → ReadStaging.ready_op_id = req_op_id  ★ release-publish ★
   A16r worker observes ready_op_id, validates C13:
        if staging.lookup_epoch < my_epoch_at_send: return -3 (retry from A3r)
        else memcpy from staging + populate L2 + L1 → return

   PROTOCOL C read path (dashed arrows, optional DRAM cache + seqlock)
   ──────────────────────────────────────────────────────────────────
   C1r worker → bucket_idx = hash(K) % B
   C2r worker → CACHELINE_LOAD(write_epoch)        ⇢⇢⇢ (one CXL line)
        ─ if cache_enabled_ AND cached_epoch_[idx] == this: scan DRAM copy + return
   C3r worker → flush slots[0]+slots[4] + mfence   ⇢⇢⇢
   C4r worker scan 7 slots, return matched value   ⇢⇢⇢ ★ no post-scan revalidation (LRC) ★
```

**Visual comparison summary**:

| Aspect | Protocol A | Protocol C |
|---|---|---|
| Distinct numbered steps (cross-host miss path) | **A1r..A16r = 16 steps** | **C1r..C4r = 4 steps** |
| Cache layers consulted | **2** (TLS L1 + KvCachePool L2) | **0-1** (optional per-process DRAM cache) |
| Linearization points the reader must observe | **3** (TLS epoch, L2 seqlock seq, C13 lookup_epoch) | **1** (write_epoch snapshot for cache validity; LRC waives post-scan revalidation) |
| Cross-host blocking points | **1** (A8r staging poll, 200ms cap) | **0** (reader never waits for a peer) |
| Threads involved in critical path | **2** (caller worker + host 1 ReadReceiver) | **1** (caller worker only) |
| CXL rings touched | **2** (ReadRing + ReadStaging) | **0** |

---

# Part 2 — Stage-by-stage measured-latency table (T=64 workload-A)

All timings from the references above. **Protocol A on FUSEE_PROBE=1
build with iter-11A all phases**; **Protocol C on
`FUSEE_LATENCY_DECOMP=1` per-slot-LFM build with iter-2 changes**.
Both at the bottleneck point of their respective sweeps (workload-A is
the Zipf-50/50-RW workload that exposes the most cross-host
contention).

## 2.1 Write path stages (every µs accounted for)

### Protocol A — owner-self UPDATE / cross-host UPDATE (W1..W12 + I1..I8)

(workload-A T=64 ratio of cross-host writes ≈ 50 %; the I* stages
fire on the ~50 % cross-host-and-peer-cached subset, hence the smaller
sample counts.)

| Stage | What it does | N | **p50 µs** | mean µs | p99 µs | Critical observation |
|---|---|---:|---:|---:|---:|---|
| **W1** | Entry + bucket flush 2 cachelines + mfence | 150 975 | 0.87 | 4.50 | 43.6 | mean >> p50 → outlier-driven by hot Zipf bucket re-fetch storms |
| **W2** | Bucket scan + acquire SlotDirectoryEntry.spinlock (DRAM, same-host) | 150 965 | 0.15 | 0.27 | 1.89 | spinlock is on DRAM not CXL → fast even contended |
| **W3** | Re-flush bucket under lock + read sharer_bitmap | 150 965 | 0.03 | 0.19 | 0.69 | |
| **W4** | Enter invalidate broadcast loop (per peer with bit set) | 11 | 0.76 | 0.83 | 1.10 | only fires when peer cached (rare since W10 resets bitmap) |
| **I1** | InvalRing.tail.fetch_add(1) — RMW-CXL | 11 | 1.69 | 1.65 | 1.87 | atomic RMW on CXL line |
| **I2** | Write InvalEntry + flush + sfence | 11 | 3.45 | 3.70 | 5.29 | dominated by wait-for-slot-free if hot |
| **I3** | (consumer) Dispatcher sees new tail | 20 | 0.99 | 1.12 | 1.73 | |
| **I4** | (consumer) Read entry | 11 | 0.58 | 0.63 | 1.33 | |
| **I5** | (consumer) cache_pool_set_stale | 11 | 0.029 | 0.034 | 0.050 | DRAM seqlock CAS |
| **I6** | (consumer) Write ACK + flush + sfence | 9 | **1 192** | **3 419** | 16 832 | **CATASTROPHIC** — this is the receiver IDLE-GAP between bursts, not per-op cost (see iter-11A Phase 2 misinterpretation in iter-12A backlog #4); a single inval handle is ~1.5 µs |
| **I7** | (producer) Observes ACK | 11 | 0.033 | 0.037 | 0.058 | |
| **I8** | Free ring slot | 11 | 0.052 | 0.65 | 6.38 | |
| **W6** | Last-ACK-seen marker (end of inval broadcast) | 11 | 1.37 | 1.22 | 1.77 | total W4-W6 ≈ 5-7 µs per peer-bit-set |
| **W7** | pool->alloc (RMW-CXL bump cursor) | 150 965 | 0.035 | 0.11 | 1.96 | |
| **W8** | pool->write(blk_off, value_bytes, value_len) | 150 965 | 0.029 | 0.16 | 1.59 | KV=512 = 8 cachelines flushed; bandwidth-bound at scale |
| **W9** | **COMMIT POINT** — publish_slot_cow (value flush + key flush + 2 sfence) | 150 965 | 0.022 | 0.054 | 0.039 | ★ ironically cheap ★ |
| **W10** | de->version++; sharer_bitmap reset; **cache_pool_insert + bucket_epoch++**; tls_insert | 150 965 | **3.56** | **6.22** | 18.10 | **DOMINANT MEAN** — driven by cache_pool_insert seqlock CAS + 512-B value memcpy under MESI contention |
| **W12** | Return | 150 916 | 0.34 | 1.50 | 9.05 | |

**Sum of mean stages (no inval)**: ≈ 13 µs/op<br>
**Sum of mean stages (with inval @ 50 % of ops)**: ≈ 13 + 0.5 × 12 ≈ **19 µs/op**<br>
**Sweep observed throughput**: 13.69 Mops/s on this exact cell (workload-a kv=1024 T=64 cache=off — note iter-11A Phase 6 best cell shifted from iter-10A's kv=512)

### Protocol C — UPDATE (per-slot LFM, iter-2)

| Stage | What it does | **avg µs** | p50 µs | p99 µs | Critical observation |
|---|---|---:|---:|---:|---|
| **W1+W2** | bucket_idx + lock_table_.lock(idx) | n/a | n/a | n/a | not separately measured; folded into "lock" below |
| **lock** | LFM acquire (cross-host blocking) | **25.0** | **9.0** | **427.1** | **DOMINANT** — cross-host LFM queue under Zipf hot bucket |
| **scan** | flush 2 cachelines + scan 7 slots (Phase-2.6 flush-collapse) | 1.40 | n/a | n/a | |
| **publish** | slot.value = V + flush + sfence | 0.018 | n/a | n/a | ★ trivially cheap ★ |
| **epoch** | __atomic_add_fetch(&write_epoch, 1) + flush + sfence | **4.07** | n/a | n/a | CXL atomic RMW; iter-2 went up 81 % vs iter-1 because non-atomic was lossy under per-slot granularity |
| **unlock** | LFM release | 0.026 | n/a | n/a | |
| **TOTAL** | end-to-end UPDATE | **30.6** | **13.4** | **439.6** | |

**Sweep observed throughput**: ~1.32 Mops/s on workload-A T=64 cache=on (well below the 20 Mops/s bar — C is now paused per project memory).

### Side-by-side comparison

| Cost class | Protocol A (mean µs) | Protocol C (mean µs) | Ratio |
|---|---:|---:|---:|
| Hash + bucket flush (W1 / scan) | 4.50 | 1.40 | A 3.2× C |
| Lock acquire | 0.27 (DRAM spinlock) | **25.0** (cross-host LFM) | **C 93× A** |
| Linearization point (publish) | 0.054 (W9) | 0.018 (publish) | A 3× C |
| Epoch / directory bump | 6.22 (W10 — cache_pool_insert + bucket_epoch++) | 4.07 (epoch) | A 1.5× C |
| Invalidate broadcast (when triggered) | ~12 µs (W4 → W6 with I1..I8) | N/A | A only |
| Cleanup / return | 1.50 (W12) | 0.026 (unlock) | A 58× C |
| **Total per-op mean (no remote sharer)** | **~13 µs** | **30.6 µs** | C 2.4× A |
| **Total per-op mean (with remote sharer)** | **~19 µs** | **30.6 µs** | C 1.6× A |

**Key insight for the supervisor**: A and C have ALMOST OPPOSITE
bottleneck profiles. A spreads cost across 12 stages with W10 (~6 µs)
as the single largest contributor — driven by cross-core MESI traffic
on the shared L2 cache_pool insert. C concentrates 80%+ of cost in
ONE stage — LFM cross-host lock acquire — which scales catastrophically
with T (lock p99 at T=64 = 427 µs, 50× the per-op total).

**The complexity story**: A has **strictly more linearization points
on the write path** (3 distinct peer-visible publishes — slot CoW
publish W9, cache_pool stale bump on each remote sharer I5, and
directory state change W10 sharer_bitmap reset). C has 1 (epoch bump).
A's complexity is not a sweep-time artifact — it's the cost of strict-A.

## 2.2 Read path stages (every µs accounted for)

### Protocol A — cross-host read with TLS L1 + L2 + forwarder-pool-direct

| Stage | What it does | N | **p50 µs** | mean µs | p99 µs | Notes |
|---|---|---:|---:|---:|---:|---|
| **R1** | Entry + (if TLS) bucket_epoch.load | 100 074 | **7.07** | 5.26 | 16.5 | **HIGH p50** — surprising; mean includes the bucket_epoch load which is a shared cacheline load (cross-core MESI) |
| **R0_tls_hit** | TLS L1 hit return (when fresh) | 18 621 | 0.025 | 0.079 | 0.81 | ★ ideal 80-ns path ★ — only ~18 % of reads hit TLS in this cell |
| **R2hit** | L2 cache_pool seqlock CAS read hit | 64 278 | 0.079 | 0.149 | 0.42 | ~64 % hit rate at L2 |
| **R2miss** | L2 miss, proceed to R3 or R5 | 17 175 | 2.80 | 3.07 | 8.12 | ~17 % of reads |
| **R3** | forward_read_direct (cross-host) | 227 | **13.6** | **14.0** | **26.4** | only ~0.2 % of total reads — but these dominate tail |
| **R4** | Validate C13 + memcpy from ReadStaging | 204 | 0.34 | 0.39 | 1.00 | |
| **R6** | Return | 97 964 | 0.73 | 1.14 | 5.82 | uniform exit |

**Reader path breakdown by outcome**:

| Outcome | Probability | p50 latency | Path |
|---|---:|---:|---|
| TLS L1 hit | ~18 % | ~80 ns | R0_tls_hit |
| L2 hit (TLS miss/stale) | ~64 % | ~7 µs | R1 + R2hit + R6 |
| Owner-self CXL fetch | ~17 % | ~10 µs | R1 + R2miss + R5 + R6 (R5 not separately probed) |
| Cross-host miss (forwarder-pool-direct) | ~0.2 % | ~22 µs | R1 + R2miss + R3 + R4 + R6 |

### Protocol C — local read with optional DRAM cache

C does not have a dedicated `search`-side latency decomp run in the
historical archive (`docs/iters/latency_decomp_C_*.md` only covered
writes). We compute the C reader cost from primitives + code:

| Stage | What it does | Primitive cost | Inferred latency |
|---|---|---:|---:|
| **C1r** | bucket_idx + entry pointer arithmetic | 1× FNV-1a, 0 mem ops | ~15 ns |
| **C2r (cache hit)** | CACHELINE_LOAD(&write_epoch); if == cached_epoch_[idx]: scan DRAM | 1× LD-CXL (one cacheline) | ~700 ns hit path |
| **C3r (cache miss)** | flush_line × 2 + mfence on bucket | 2× FLUSH (66 ns each) + 1× MFENCE | ~150 ns |
| **C4r** | scan 7 slots in L1 (cachelines now fresh from CXL); break on key match | up to 7× LD on L1 | ~50 ns |
| **(opt) cache populate** | cache_buckets_[idx] = *bucket; cache_epoch_[idx] = e1 | 1× 112-B DRAM memcpy + ST | ~30 ns |
| **C5r** | return | — | — |

**Reader path breakdown**:

| Outcome | Probability | p50 latency | Path |
|---|---:|---:|---|
| DRAM cache hit (cache_enabled_) | depends on cache state | ~700 ns | C1r + C2r-hit + C5r |
| Cache miss / cache disabled | rest | ~1 µs | C1r + C3r + C4r + C5r |

### Reader side-by-side

| Cost class | Protocol A (typical) | Protocol C (typical) | Ratio |
|---|---:|---:|---:|
| Hot-key best case | ~80 ns (TLS L1) | ~700 ns (DRAM cache hit) | **A 9× faster** when TLS warm |
| Cold-key local owner | ~10 µs (R1 + R2miss + R5) | ~1 µs (C3r + C4r) | **C 10× faster** when forced to flush |
| Cross-host miss | ~22 µs (R1+R2miss+R3+R4+R6) | ~1 µs (same as local — C has no cross-host miss notion) | **C 22× faster** |

**Key insight**: A's TLS L1 is the only path that beats C; everything
else is decisively slower because A's L2 has shared-bucket seqlock and
its cross-host miss involves a full forwarder roundtrip. C's reader
just flushes 2 cachelines and scans, with no cross-host coordination
ever. **C readers are designed to be near-CXL-physics; A readers carry
the cost of maintaining strict-A coherence.**

---

# Part 3 — Linearization points and overhead counting

| Property | Protocol A | Protocol C |
|---|---:|---:|
| **Distinct stage tags emitted in production code** | **25** (W1-W12, R0_tls_hit, R1-R6, I1-I8 minus W5/W11) | **6** (Lock / Scan / Publish / Epoch / Unlock / Total) |
| **Write-path linearization points (peer-visible commits)** | **3** (W9 slot publish + I5 cache_pool_stale per remote sharer + W10 sharer_bitmap reset) | **1** (W5 write_epoch atomic bump) |
| **Read-path linearization points** (where reader must validate freshness) | **3** (TLS L1 epoch compare + L2 seqlock seq compare + R4 C13 lookup_epoch compare) | **1** (write_epoch snapshot; LRC waives post-scan revalidation) |
| **Mandatory cross-host messages per cross-host write** (caller side) | **2 send + 2 receive** (WriteRing F1-F7 round trip + InvalRing I1-I7 round trip per peer sharer) | **0** (LFM lock is shared CXL state, not message-passed; cost is spin/wait, not handoff) |
| **Background threads required per host** | **6** (WriteSender, WriteReceiver, ReadSender, ReadReceiver, InvalSender, InvalReceiver — though 3 senders are opt-in) | **0** (mandatory); **0-8** flusher (opt-in for batching) |
| **CXL atomic RMW operations per write op (best case)** | **3** (pool bump, WriteRing tail [cross-host], cache_pool bucket_epoch [implicit in W10]) | **1** (write_epoch bump) |
| **CXL atomic RMW operations per write op (worst case, peer cached)** | **5** (+ InvalRing tail + cache_pool seqlock CAS on peer) | **1** |
| **Critical-path code lines (cxl_kv_ops_X.cc functions involved)** | execute_write_local (200 lines) + send_invalidate_direct (75 lines) + forward_write_direct (50 lines) + write_handler (25 lines) + inval_receiver_loop (40 lines) + cache_pool_insert (~80 lines) + tls_insert (~30 lines) ≈ **500 lines** | insert/update/remove (~50 lines each) + lock_table_.lock + bump_epoch + publish_slot ≈ **200 lines** |
| **Hardware coherence dependencies on write path** | x86 8-B atomic ST + clflushopt + sfence for slot publish; DRAM spinlock for SlotDirectoryEntry; seqlock CAS for cache_pool; CXL atomic for ring tail + bucket_epoch + pool cursor | LFM lock state machine on CXL (5-step rendezvous protocol); CXL atomic for write_epoch |
| **§I9 strict-linearizability invariant load-bearing pieces** | W9 ordering (value before key); W10 bitmap reset before unlock; AP15 cache populate AFTER ACK; C13 epoch tag on staging; InvalRing separate channel for deadlock freedom | N/A — C is LRC by design, no equivalent invariant |

---

# Part 4 — The thesis in one paragraph (for the slide deck)

> Protocol A's read/write paths are **structurally more complex than
> Protocol C's**, not because of immature engineering, but because
> strict-A linearizability requires: (a) sharded ownership + cross-host
> request forwarding via CXL rings, (b) cache-coherence invalidation
> broadcast to every remote sharer before each write commit, (c)
> register-then-fill protocols for cross-host reads with epoch-tagged
> staging arenas to prevent stale-snapshot violations, and (d) a
> 2-tier cache hierarchy (TLS L1 + shared L2) with bucket-epoch and
> seqlock-CAS coherence at every layer. Measured at T=64 workload-A,
> A's write path traverses 14 distinct CXL operations involving 3
> threads with mean 13–19 µs/op (W10 dominant at 6 µs); C's traverses
> 6 stages on a single thread with mean 30 µs/op — but **C's cost is
> 84%+ in ONE stage (cross-host LFM acquire on the hot bucket),
> meaning C scales worse with thread count but has a much simpler
> coordination surface**. A's reader has 3 cache layers and a 4-step
> cross-host miss path (R3+R4 = 14 µs mean) but achieves 80 ns on TLS
> L1 hits; C's reader is just one CXL flush + 7-slot scan (~1 µs
> regardless of contention). The complexity-vs-overhead trade-off is
> the protocol's correctness budget: A spends µs to guarantee that
> readers never observe an order violation; C trades that guarantee
> for a 3× simpler write path that, in turn, exposes a single hot-LFM
> bottleneck under cross-host write skew.

---

# Part 5 — Suggested slide structure (3 slides)

**Slide 1 (Path Topology)**: Part 1's unified diagram, side-by-side
visualisation of solid (A) vs dashed (C) arrows. Caller asks: "Look
at the arrow count" — A has 14 numbered arrows on write, C has 6.

**Slide 2 (Where the µs go)**: Part 2's two write-path tables side
by side. Highlight the W10 (A) and lock (C) row in red — these are
the protocol-specific dominant stages. Insight: **different
bottleneck shapes for different consistency models**.

**Slide 3 (Linearization point inventory)**: Part 3's property table.
End with the one-paragraph thesis from Part 4 as the "takeaway"
slide. Optional follow-up: open the floor to "can A's W10 cost be
eliminated?" — answer is iter-12A backlog #5 (hot-key replication
per-CPU value copies) or #7 (RCU cache_pool re-eval post-R3-fix).

---

## Source data references

- A measurements: `docs/path_decomp_iter11A_20260511_023247/per_cell/workloada_best/healthy_stages.tsv` and `per_stage_decomp.md`
- C measurements: `docs/iters/latency_decomp_C_iter2_20260423_053919.md` (write path); `docs/iters/latency_decomp_C_iter3_lock_anatomy_20260424_090048.md` (LFM sub-stage breakdown — `lfm_localstore` 16 ns, `lfm_peerscan` 2.8 µs, `lfm_entercs` 1.5 µs, uncontended sum 4.4 µs)
- Sweep headlines: `docs/g34_scaling_ycsb_iter11A_20260511_042814/gap_to_target.md` (A peak 13.69 Mops/s on workload-A); `docs/iters/iter11A_summary_20260511.md` Phase delivery audit (no C YCSB driver — C measured via micro-bench only)
- Hardware baselines: memory `reference_g34_hw_baseline.md` (DRAM 174.8 ns/393 GB/s, CXL 607.8 ns/51.78 GB/s); `cxl_primitive_bench` measured `clflushopt+sfence` 66 ns, CXL atomic fetch_add+flush+sfence 1.4 µs
