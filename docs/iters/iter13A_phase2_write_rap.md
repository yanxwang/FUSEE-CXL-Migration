# iter-13A Phase 2 — Write-path RAP (W1 per-host-reserved vs W3 batched pre-allocation)

**Date**: 2026-05-17
**Branch**: `feat/cxl-migration`
**Parent**: `docs/iters/task_plan_iter13A.md §Phase 2`
**Goal**: 完全消除 cross-host 写路径上 worker→staging + receiver→pool 的两次拷贝；让 worker 一步到位直接写入 owner 的 blockpool，receiver 只更新 bucket pointer。

---

## STATE (≤ 30 chars)

> Direct cross-host pool write, no staging.

## 当前写路径 vs 目标写路径

### 当前 (STAGING)
```
Worker (host A) forward_write_direct(key, value):
  1. fetch_add(WriteRing[A][B].tail) → reserve slot
  2. memcpy(forward_staging[A][B][slot], value, vlen)  ← DRAM→CXL #1
  3. flush_lines + sfence
  4. publish WriteEntry.req_op_id

Receiver (host B) write_handler(req):
  a. flush + load staging value bytes               ← CXL→DRAM #2
  b. pool_->alloc() → blk_off in B's local segment
  c. pool_->write(blk_off, staging_value, vlen)     ← DRAM→CXL #3
  d. bucket->slots[i].value = encode(blk_off, vlen)
```
**冗余**: same value bytes 经过 CXL 三次 (DRAM→staging→DRAM→pool). 同一字节流写两次到 CXL.

### 目标 W1 或 W3
```
Worker (host A) forward_write_direct(key, value):
  1. blk_off = local_alloc_peer(B)                  ← bump in DRAM, no CXL atomic
  2. pool_->write(blk_off, value, vlen)             ← DRAM→CXL ONCE, into B's segment
  3. fetch_add(WriteRing[A][B].tail) → reserve slot
  4. WriteEntry.blk_off = blk_off; key = key; vlen = vlen
  5. publish WriteEntry.req_op_id

Receiver (host B) write_handler(req):
  a. bucket->slots[i].value = encode(req.blk_off, req.vlen)   ← no value-bytes copy
  b. (GC) retire old blk_off if existed
```
**Saved**: 2 of 3 CXL data transits per cross-host write. For workloada T=64 kv=1024 (~5% writes × 50% cross-host × 1024B), saves ~5 KB CXL bandwidth per 1000 ops.

---

## 候选 W1: Per-host reserved segments

### Pool layout change

```
CxlKvBlockPool (logical view, 2-host):

  pool_base
  ├─ host_0_segment  (size = num_blocks_per_host × block_size)
  │   ├─ private_part  [bump_self_0]              (70% of segment)
  │   └─ reserved_by_1 [bump_peer_0_in_1]         (30% — host 1 bumps here when writing host-0 keys)
  └─ host_1_segment
      ├─ private_part  [bump_self_1]              (70%)
      └─ reserved_by_0 [bump_peer_1_in_0]         (30% — host 0 bumps here when writing host-1 keys)
```

- `bump_self_h` lives in **CXL** (cross-host coherent via flush_line — same as today)
- `bump_peer_a_in_b` lives in **host-a's DRAM** (only host A advances; never read by host B's allocator). Host B only reads blk_offs that A produced via WriteRing — no need for B to see A's bump pointer state.

### API additions (in `src/cxl_kv_blockpool.{h,cc}`)

```cpp
class CxlKvBlockPool {
 public:
  // Existing: alloc() — bumps cursors_[host_id_], returns blk_off in own segment.
  uint64_t alloc();  // == alloc_local()

  // iter-13A Phase 2 W1: alloc in OWNER host's "reserved-for-me" sub-segment.
  // Bump pointer is local-DRAM (no CXL atomic). Returns 0 on segment exhaust.
  uint64_t alloc_peer(int owner_host);

  // iter-13A: retire a block (push onto retire list; actual free deferred
  // to rcu_synchronize). Only valid if blk_off was allocated by this host.
  void retire(uint64_t blk_off);
};
```

### GC (G1 RCU-defer + retire list)

Per user QR6 decision: blocks are reclaimed by the host that allocated them. So:
- host A retires blocks A allocated (whether in own private or in B's reserved-for-A sub-segment)
- Retire list is per-host DRAM
- When retire list crosses threshold OR a bump cursor approaches segment end: `rcu_advance_epoch()` → `rcu_synchronize()` → free retired blocks back to a per-segment freelist; OR rewind bump pointer if retire list ≥ segment size

For iter-13A 200k-op tests, bump pointer never wraps (capacity exceeds workload by 10×+). GC code paths SHIP but the rcu_synchronize is exercised only on the slow path; warm path is pure bump. Acceptable.

### Compile flag

`-DFUSEE_WRITE_ALLOC=1` (== `FUSEE_WRITE_ALLOC_RESERVED`)

---

## 候选 W3: Batched pre-allocation

### Concept

Each worker thread maintains a per-thread DRAM queue of pre-reserved blk_offs from each peer host. On write to owner B:
- pop blk_off from queue[B]
- pool_->write(blk_off, value, vlen)
- send (key, blk_off, vlen) on WriteRing

When queue[B] is empty (or low water mark), send a `RESERVE_REQUEST(K)` to owner B; owner allocates K blocks from its private segment, returns the K blk_offs.

### New structures

```cpp
struct ReservationRingEntry {     // CXL
  std::atomic<uint64_t> req_op_id;
  uint32_t K;                      // requested batch size
  std::atomic<uint64_t> resp_op_id;
  uint64_t blk_offs[kMaxBatchK];  // owner fills in response
};

struct ReservationRingMatrix {
  ReservationRing rings[kMaxHosts][kMaxHosts][kReservRingDepth];
};

// Per-thread DRAM queue
struct ReservedQueue {
  uint64_t blk_offs[kMaxBatchK];
  int next_use_idx;
  int filled_count;
};
```

### Hot path
- worker_thread_local `ReservedQueue queue[NUM_HOSTS]`
- For owner B: if `queue[B].filled_count == 0`: send reservation request, wait for response
- Otherwise: pop one blk_off, decrement count

### Batch size K — per QR4 sweep

K not pre-set; runs Phase 2.2.E sweep on workloada × T={4,16,32,64} × K={16, 32, 64, 128, 256, 512, 1024, 2048, 4096} × 3 reps → 108 runs → pick K_winner by median throughput + p99 constraint.

### Compile flag

`-DFUSEE_WRITE_ALLOC=2` (== `FUSEE_WRITE_ALLOC_BATCHED`)

---

## RAP §XIII

### V_PERFORMANCE

**Attack 1**: W1 vs W3 — synchronization cost?
- **W1**: ZERO cross-host coordination on hot path. Bump is local DRAM atomic.
- **W3**: First write to a new owner pays a round trip (CXL send + spin + CXL read). Subsequent K-1 writes are free. Avg amortized = round-trip / K.
- For workloada 50%U with 50% cross-host: each worker issues ~1000 cross-host writes per rep. K=16 means ~62 round trips per worker = ~62 × 10 µs = 620 µs total stalls per worker. K=4096 → ~1 round trip per worker → ~10 µs total stalls. Big swing.
- **Verdict**: W1 simpler and faster on hot path; W3 wins only if pre-allocation amortization beats the K=1 W1 baseline (which it can't, since W1 has zero stall).

**Attack 2**: W1 allocates from a fixed-size 30% reserved segment. If exhausted → fallback. Is the fallback graceful?
- **Defense**: Fallback = old STAGING path. Performance reverts to baseline for that specific write. fprintf warning + counter in iter summary. For iter-13A workloads (200k ops × 5% writes × 50% cross-host × 1024B = ~5 MB peer write demand), 30% of ~1 GB pool = 300 MB >> 5 MB → never exhausts in tests.
- **Verdict**: safe fallback.

**Attack 3**: Per-thread reserved queue (W3) — memory cost?
- **Defense**: kMaxBatchK = 4096; sizeof(blk_off) = 8 B → 32 KB per thread × NUM_HOSTS-1 = 32 KB per worker for 2-host. Workers × 32 KB = 64 × 32 KB = 2 MB total per host. Trivial.
- **Verdict**: ok.

### V_CORRECTNESS (§I9)

**Attack 4**: Worker writes value to pool BEFORE publishing req_op_id. If receiver sees req_op_id but worker's pool->write hasn't completed (CXL re-order), receiver reads garbage.
- **Defense**: Worker does `pool_->write` then `flush_line` on every cacheline then `store_fence` then `req_op_id.store(release)` then `flush_line(&req_op_id)` then `store_fence`. The release fence + flush_line ensures pool bytes are durable on CXL before req_op_id is visible to receiver.
- Receiver does: flush_line + full_fence + load(req_op_id); if non-zero, immediately reads bucket->slot's encoded blk_off; reader downstream (forward_read_direct in HAZARD build) flushes the pool bytes before pool_->read. Same fencing as iter-12A Phase 5 stale-cache rule.
- **Verdict**: §I9 preserved.

**Attack 5**: Two writers race to the same key? (Both A and B issue writes for same key, but different owners — impossible since key hashes to single owner.)
- **Defense**: owner_host(key) deterministic; only ONE host's writers ever write a given key cross-host. Local writes use execute_write_local which has bucket lock. No race.
- **Verdict**: no race.

**Attack 6**: Sequence of writes to same key from same host — earlier write's block could be retired before later write reads it?
- **Defense**: Same key write sequence is serialized through the per-host bucket lock in receiver's execute_write_local. Old blk_off retire happens AFTER new blk_off becomes the bucket pointer. Any reader observing the old blk_off had to capture it BEFORE the new write committed (via slot pointer), and is protected by HAZARD slot.
- **Verdict**: race-free under existing slot-pointer + HAZARD discipline.

### V_GENERALITY

**Attack 7**: W1 capacity split (70/30) — what if a workload is write-heavy AND skewed (most writes go to one peer)?
- **Defense**: workloada is 50/50; even at 50% write, with 200k ops × 50% × 50% cross-host × 1024B = 50 MB peer writes per host per rep. 30% of (say) 8 GB pool = 2.4 GB. Burn-down rate: 50 MB / 200k ops = 250 B/op average. 2.4 GB / 250 B = ~10M ops before exhaustion. iter-13A tests don't reach that.
- **Verdict**: scope-safe for iter-13A; production tuning is iter-14A backlog.

**Attack 8**: W3 K sweep on workloada might pick a K that's bad for other workloads.
- **Defense**: workloadc has zero writes — K doesn't matter. workloadb (95R/5U) has few writes → both K=16 and K=4096 amortize fine. workloadf (50R/50RMW) has 50% writes — sensitive like workloada. So workloada sweep generalizes to f, and is irrelevant for b/c/d.
- **Verdict**: workloada K sweep is the right proxy.

### V_COMPLEXITY

**Attack 9**: Implementation footprint?
- W1: ~150 LOC blockpool extension + ~80 LOC retire list + ~30 LOC forward_write_direct edit + ~20 LOC write_handler edit = ~280 LOC.
- W3: ~200 LOC ReservationRing type + ~100 LOC owner reservation handler thread + ~60 LOC worker queue management + ~30 LOC forward_write_direct edit = ~390 LOC.
- W1 simpler. Per task plan §C5 both within iter-13A scope.

### V_PRIOR_ART

- **W1 per-segment private/reserved**: standard DPDK mempool pattern (per-lcore private pool + cross-core ring).
- **W3 batched pre-allocation**: malloc/free arenas (tcmalloc per-thread caches with refill rate), per-CPU slab allocators (SLUB).
- Both proven; W1 cleaner for shared-memory cross-process.

### V_IMPLEMENTATION_FEASIBILITY

- Both compile-time selected via `FUSEE_WRITE_ALLOC` flag, parallel to `FUSEE_READ_GUARD`.
- 3 build dirs: build-cxl (STAGING/STAGING — baseline), build-cxl-w1 (HAZARD/RESERVED), build-cxl-w3 (HAZARD/BATCHED).

### V_DIAGNOSTIC_PROVENANCE

- Phase 2.3 5-cell × 5-rep compare with measured median throughput + w_avg + w_p99 → number-vs-number pick.
- Phase 2.2.E K sweep produces choosing K_winner by ranked thpt + p99 constraint, NOT by intuition.
- All conclusions reference TSV data files.

---

## ABLATION

Phase 2 winner sweep is compared against:
- iter-12A baseline (Phase 0 sweep): isolate combined R+W gain
- iter-13A Phase 1 sweep: isolate Phase 2 incremental gain (read-path already cleared)

Both deltas reported in `iter13A_phase2_summary.md`.

---

## DECISION

**实现 W1 + W3 双轨，跑代表性 cell 对比，按数字 pick winner**.

Implementation order:
1. Phase 2.1 W1 first (simpler, has confidence in approach)
2. Phase 2.2 W3 + 2.2.E K sweep on workloada
3. Phase 2.3 dual-track compare with W3 using K_winner from 2.2.E
4. Phase 2.4 G1 hash-diff on winner
5. Phase 2.5 full sweep + plots

Winner pick rule (per task plan §Phase 2.3):
1. **Primary**: median throughput on workloada T=64 cache=off kv=1024 (write-heavy, the worst-gap cell)
2. **Tie-break**: median w_p99 across 5 cells
3. **Veto**: w_avg regress > 30 % from STAGING any cell
