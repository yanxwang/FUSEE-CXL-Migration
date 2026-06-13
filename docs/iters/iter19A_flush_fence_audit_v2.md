# Protocol A — flush + fence comprehensive audit (4 paths + other)

**Purpose**: complete catalogue of every active `flush_line` /
`store_fence` / `full_fence` in Protocol A source, organized by the
four user-op hot paths plus an "other" group for sites not on those
paths. For each site we record:

- **Content flushed**: what memory address is being flushed; what data
  lives there.
- **Fence order**: which set of operations is the fence enforcing in
  program order (what's before / after).
- **Cross-host visibility**: does another host's read of the flushed
  content depend on this flush?
- **Order necessity**: does another host (or thread on the same host)
  depend on the order this fence establishes?

**Verdict legend**:
- ✅ Required (removing breaks correctness).
- ⚠ Conditionally required (depends on the cross-host code path being
  exercised).
- ❌ Removable (no observer depends on it).
- 🧪 Already gated by an isolation flag — verdict pending Task 3c data.

Sources scanned: [src/cxl_kv_ops_A.cc](../../src/cxl_kv_ops_A.cc),
[src/cxl_kv_blockpool.cc](../../src/cxl_kv_blockpool.cc),
[src/cxl_read_guard.h](../../src/cxl_read_guard.h),
[src/cxl_pending_ring.h](../../src/cxl_pending_ring.h).

---

## Path 1 — local_read (LR)

Function: `search()` owner-self HIT or MISS. Callees: `cache_pool_lookup`
(DRAM), `pool->read` (CXL block), `tls_lookup/insert` (DRAM).

### LR.1 — `cxl_kv_ops_A.cc:3052-3056` bucket pre-scan flush (LRS3)

```cpp
#if !FUSEE_LR_DEL_OWNER_FLUSH       // ← gated, default OFF in bnoflush
#if !FUSEE_LR_DEL_FLUSH_ONLY
  flush_line(bucket);               //  → LR.1.a
  flush_line((char *)bucket + 64);  //  → LR.1.b
#endif
#if !FUSEE_LR_DEL_FENCE_ONLY
  full_fence();                     //  → LR.1.c
#endif
#endif
```

| Site | Content flushed | Fence order |
|---|---|---|
| LR.1.a | CXL bucket cacheline 1 (slots 0-3, 64 B) | — |
| LR.1.b | CXL bucket cacheline 2 (slots 4-7, 64 B) | — |
| LR.1.c | full_fence (mfence) | LR.1.a/b clflushopt → subsequent slot-key load |

**Cross-host visibility?** No: owner-self path; only the owner host
writes its own buckets. MOESI ensures owner's L1 has fresh copy.

**Order necessity?** No: nothing peer-visible follows.

**Verdict**: ❌ Removable. Already removed in bnoflush (FUSEE_LR_DEL_OWNER_FLUSH=1).
Validated iter-19A Phase 2 with 5.9× thpt gain at zipf-1.5.

### LR.2 — `cxl_kv_blockpool.cc:read()` per-cacheline block flush (LRS3)

```cpp
#if !FUSEE_LR_DEL_POOL_READ_FLUSH  // ← gated, NEW Task 3c-G2
  while (p < end) { flush_line(p); p += 64; }
  full_fence();
#endif
```

| Site | Content flushed | Fence order |
|---|---|---|
| LR.2.a..p | CXL block bytes, V=1024 → 16 cachelines | — |
| LR.2.q | full_fence (mfence) | LR.2.a..p clflushopt → `memcpy(out, src, len)` |

**Cross-host visibility?** No on owner-self MISS — the owner wrote the
block via `pool->write` from `execute_write_local`, and MOESI keeps
owner's L1 fresh. **YES** if the same `pool->read` is invoked for a
peer-host-written block (xhost write where peer host did the
`pool->write`).

**Order necessity?** No on owner-self; YES on peer-written reads.

**Verdict**: 🧪 Conditional on owner-vs-peer source of the block.
Task 3c-G2 isolation enabled in build `bnf-G2`. Theoretical risk: tiny
(local_read MISS reads only own-written blocks, since peer keys go
through forward_read which uses a different cache_pool register path).

---

## Path 2 — local_write (LW)

Function: `execute_write_local()` — INSERT / UPDATE / DELETE.

### LW.1 — `cxl_kv_ops_A.cc:317-318` retire_slot flush (LWS4 DELETE)

```cpp
inline void retire_slot(CxlKvSlot *slot) {
  __atomic_store_n(&slot->key, kEmptyKey, __ATOMIC_RELEASE);
#if !FUSEE_LW_DEL_RETIRE_FLUSH     // ← gated, NEW Task 3c-G3
  flush_line(slot);
  store_fence();
#endif
}
```

| Site | Content flushed | Fence order |
|---|---|---|
| LW.1.a | CXL slot's 64 B cacheline (now contains key=kEmptyKey + leftover value) | — |
| LW.1.b | store_fence (sfence) | LW.1.a clflushopt → any later peer-visible event (next op on this thread, or peer's incoming OP_INVALIDATE delivery) |

**Cross-host visibility?** YES — peer hosts that previously cached
this key need to see the DELETE. Without it: peer reads stale value.

**Order necessity?** YES — LW.1 must happen before LWS3 sharer-invalidate
broadcast triggers OP_INVALIDATE delivery to peers. (Otherwise peer's
ACK-handling could re-cache the stale slot.)

**Verdict**: ⚠ Conditionally required for cross-host correctness.
Removable in single-host configuration. Task 3c-G3 isolation enabled in
`bnf-G3` to quantify thpt cost — but production cannot ship this flag
without an alternative cross-host invalidation guarantee.

### LW.2 — `cxl_kv_ops_A.cc:309-310` publish_slot_cow flush (LWS4 inline-u64)

```cpp
inline void publish_slot_cow(CxlKvSlot *slot, uint64_t key, uint64_t encoded_value) {
  slot->value = encoded_value;
  slot->key = key;
#if !FUSEE_LW_DEL_SLOT_PUB_FLUSH    // ← gated, NEW Task 3c-G45
  flush_line(slot);
  store_fence();
#endif
}
```

| Site | Content flushed | Fence order |
|---|---|---|
| LW.2.a | CXL slot's 64 B cacheline (slot.value + slot.key just written) | — |
| LW.2.b | store_fence | LW.2.a clflushopt → peer's observation of new key/value |

**Cross-host visibility?** YES — peers that previously cached this key
(or peers that will look this key up via forward_read) need to see the
new value bytes / key. The CXL slot is the authoritative storage.

**Order necessity?** YES — same reason as LW.1.

**Verdict**: ⚠ Conditionally required for cross-host correctness.

### LW.3 — `cxl_kv_ops_A.cc:309-310` publish_slot_cow flush (LWS4 blockpool)

Same code site as LW.2, but reached on the main blockpool path where
`encoded_value = cxl_slot_pack(blk_off, sc, fp)` (the slot now points to
a block_offset in the CXL pool, not an inline value).

**Cross-host visibility?** YES — peers need to see the new (blk_off, key)
pair so they can `pool->read(blk_off)` and get the new value.

**Order necessity?** Critical ordering: **the CXL block bytes (LW.4)
MUST be flushed before LW.3 makes the slot point to them.** If a peer
sees the new slot but the block bytes have not yet propagated, the peer
reads stale/garbage from `pool->read`. The store_fence at LW.3.b
guarantees pool->write completes (LW.4 sfence retired) before LW.3
clflushopt is observable.

**Verdict**: ⚠ Conditionally required for cross-host correctness.
Same Task 3c-G45 flag gates this with LW.2.

### LW.4 — `cxl_kv_blockpool.cc:write()` per-cacheline block flush (LWS4 blockpool only)

```cpp
#if !FUSEE_LW_DEL_POOL_WRITE_FLUSH  // ← gated, NEW Task 3c-G6
  while (p < dst + len) { flush_line(p); p += 64; flushed += 64; }
  store_fence();
#endif
```

| Site | Content flushed | Fence order |
|---|---|---|
| LW.4.a..p | CXL block bytes, V=1024 → 16+1 cachelines (4 B header + 1024 B value, page-aligned) | — |
| LW.4.q | store_fence | LW.4.a..p clflushopt → LW.3 slot publish (must complete before slot points here) |

**Cross-host visibility?** YES — peers reading via forward_read +
cache_pool_register receive the new block bytes via this flush.

**Order necessity?** YES — must precede LW.3 slot publish for safe
cross-host read; see LW.3 analysis.

**Verdict**: ⚠ Conditionally required for cross-host correctness.

### LW.5 — `send_invalidate` flush from LWS3 (no direct flush in worker)

The worker writing `s->state.store(kAggrPending)` is a DRAM AggrSlot
write. The actual CXL InvalRing flush+sfence happens in the sender
thread (see "Other" group below). **No flush+fence appears on the
worker's LW path for stage 3 itself.**

---

## Path 3 — xhost_read (XR sender + XR receiver)

### XR sender — `forward_read_direct()` (lines 2261-2434)

#### XR.S1 — ring tail flush (XRS1 slot_reserve)

```cpp
flush_line((void *)&ring->tail);   // L2281
store_fence();                     // L2282
```

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S1.a | CXL ring->tail counter (after sender's fetch_add) | — |
| XR.S1.b | store_fence | XR.S1.a → next op on this slot |

**Cross-host visibility?** YES — owner-host receiver reads `ring->tail`
to know how many slots are pending.

**Order necessity?** YES — receiver must observe updated tail before any
later observation by the same sender (e.g. recipient re-reads tail
during gap-tolerance).

**Verdict**: ✅ Required for the cross-host RTT to advance.

#### XR.S2 — per-slot req_op_id flush + mfence (XRS2 slot_wait spin)

```cpp
flush_line((void *)&e->req_op_id); // L2291
full_fence();                      // L2292
```

Inside the `while (req_op_id != 0) flush+load+pause` spin.

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S2.a | CXL ReadEntry cacheline 1 (req_op_id atomic) | — |
| XR.S2.b | full_fence (mfence) | XR.S2.a clflushopt → following acquire load |

**Cross-host visibility?** YES — sender is waiting for prior owner of
this slot (a different worker, possibly different host) to release.

**Order necessity?** YES — load must re-fetch from CXL, not stale L1.

**Verdict**: ✅ Required.

#### XR.S3 — ready_op_id pre-clear flush (XRS3 req_publish)

```cpp
flush_line(&st->ready_op_id);      // L2324
```

(no fence here — final fence at L2334.)

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S3.a | CXL ReadStaging->ready_op_id atomic (clearing to 0) | — |

**Cross-host visibility?** YES — receiver will eventually write ready_op_id=op_id; sender must clear first so receiver's write is observable as a new value.

**Order necessity?** Yes — must precede the req_op_id publish at XR.S4 (otherwise receiver could pick up the req before our staging slot is in a clean state). Ordering is enforced by the single sfence at XR.S4.b below.

**Verdict**: ✅ Required.

#### XR.S4 — req publish flush + sfence (XRS3)

```cpp
flush_line((void *)&e->req_op_id); // L2333
store_fence();                     // L2334
```

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S4.a | CXL ReadEntry cacheline 1 (req_op_id atomic, now = op_id) | — |
| XR.S4.b | store_fence | XR.S3.a (staging clear) + XR.S4.a (req publish) → receiver's pickup |

**Cross-host visibility?** YES — receiver reads req_op_id to dispatch.

**Order necessity?** YES — staging must be clean BEFORE receiver sees req
(otherwise receiver might read stale staging and ACK with wrong data).

**Verdict**: ✅ Required.

#### XR.S5 — ack-wait spin flush + mfence (XRS4 ack_wait, dominant stage)

```cpp
flush_line(&st->ready_op_id);      // L2348
full_fence();                      // L2349
```

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S5.a | CXL ReadStaging->ready_op_id (waiting for receiver to set it) | — |
| XR.S5.b | full_fence | XR.S5.a clflushopt → following acquire load |

**Cross-host visibility?** YES — sender is waiting for receiver-side
write to ready_op_id.

**Order necessity?** YES — load must re-fetch from CXL.

**Verdict**: ✅ Required. (Could be `lfence` instead of `mfence` — see
iter-17A Stage 5 micro-opt at the analogous local position. Saves
10-20 ns/spin-iter. ❓ candidate for follow-up.)

#### XR.S6 — req cleanup flush + sfence (XRS5 cleanup_validate)

```cpp
flush_line((void *)&e->req_op_id); // L2371
store_fence();                     // L2372
```

Writes req_op_id = 0 to free the ring slot for next sender.

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S6.a | CXL ReadEntry cacheline 1 (req_op_id = 0) | — |
| XR.S6.b | store_fence | XR.S6.a → next sender's XR.S2 spin |

**Cross-host visibility?** YES — next sender on this slot reads req_op_id.

**Order necessity?** YES.

**Verdict**: ✅ Required.

#### XR.S7 — value bytes flush + mfence (XRS6 value_recv)

```cpp
flush_line(st->value_bytes + off); // L2426 (per cacheline)
full_fence();                      // L2428
```

| Site | Content flushed | Fence order |
|---|---|---|
| XR.S7.a..p | CXL ReadStaging->value_bytes 16 cachelines | — |
| XR.S7.q | full_fence | XR.S7.a..p → memcpy out |

**Cross-host visibility?** Reverse direction — sender reads receiver's
write. Need to clflushopt to re-fetch from CXL.

**Order necessity?** YES — load must re-fetch.

**Verdict**: ✅ Required. (Same lfence-vs-mfence opportunity as XR.S5.)

### XR receiver — `read_receiver_loop()` + `read_handler()`

#### XR.R1 — ring tail load flush + mfence (XRR1 ring_drain)

```cpp
flush_line((void *)&ring->tail);   // L2821
full_fence();                      // L2822
```

**Cross-host visibility?** Receiver reads sender's tail write.

**Order necessity?** YES — load must re-fetch.

**Verdict**: ✅ Required.

#### XR.R2 — per-slot req_op_id flush + mfence (XRR1 ring_drain)

```cpp
flush_line((void *)&e->req_op_id); // L2827, L2840
full_fence();                      // L2828, L2841
```

Two sites: initial load, and gap-tolerance re-check.

**Cross-host visibility?** Receiver reads sender's req_op_id publish.

**Order necessity?** YES.

**Verdict**: ✅ Required.

#### XR.R3 — bucket flush + mfence (XRR2 handler, bucket scan)

```cpp
flush_line(bucket);                  // L2607, L2626
flush_line((char *)bucket + 64);     // L2627
full_fence();                        // L2608, L2628
```

**Cross-host visibility?** Receiver is on owner host reading owner's own
bucket. **MOESI covers** — same MOESI argument as LR.1.

**Order necessity?** No peer observes anything after this load — the
receiver immediately uses the value internally.

**Verdict**: ❌ Removable — analogous to LR.1 / B-H3. Equivalent of
"xhost read receiver owner-self flush" — not yet isolated (could be
G2_recv counterpart). Theoretical risk: zero.

#### XR.R4 — staging clear flush + sfence (XRR2 handler, NOOP path)

```cpp
flush_line(st_noop);               // L2596, L2615
store_fence();                     // L2597, L2616
flush_line(&st_noop->ready_op_id); // L2599, L2618
store_fence();                     // L2600, L2619
```

For NOOP (key not found) path — writes status + ready_op_id back to sender.

**Cross-host visibility?** YES — sender reads ready_op_id.

**Order necessity?** YES — staging state must be visible BEFORE ready_op_id flip.

**Verdict**: ✅ Required.

#### XR.R5 — staging value bytes publish flush + (no fence here, fenced at R6)

```cpp
flush_line(st);                          // L2643 (header)
flush_line(st->value_bytes + off);       // L2649 (per cacheline)
store_fence();                           // L2657
```

| Site | Content | Fence order |
|---|---|---|
| XR.R5.a | CXL staging header + cachelines a..p | — |
| XR.R5.b | store_fence | XR.R5.a → XR.R6 (ready_op_id publish) |

**Cross-host visibility?** YES — sender reads these bytes.

**Order necessity?** YES — value bytes MUST be visible before ready_op_id flip.

**Verdict**: ✅ Required.

#### XR.R6 — ready_op_id publish flush + sfence (XRR3 ack_publish)

```cpp
flush_line(&st->ready_op_id);      // L2661
store_fence();                     // L2662
```

**Cross-host visibility?** YES — sender's XR.S5 spin reads this.

**Order necessity?** YES.

**Verdict**: ✅ Required.

#### XR.R7 — resp_op_id flush + sfence (XRR3 also, for direct case)

```cpp
flush_line((void *)&e->resp_op_id);// L2863
store_fence();                     // L2864
```

**Cross-host visibility?** YES.

**Order necessity?** YES.

**Verdict**: ✅ Required.

---

## Path 4 — xhost_write (XW sender + XW receiver)

### XW sender — `forward_write_direct()` (lines 1721-1911)

#### XW.S1 — ring tail flush + sfence (XWS1 slot_reserve)

```cpp
flush_line((void *)&ring->tail);   // L1733
store_fence();                     // L1734
```

**Verdict**: ✅ Required (analog of XR.S1).

#### XW.S2 — slot_wait flush in spin (XWS2)

```cpp
flush_line((void *)&e->req_op_id); // L1747 (in spin, no fence — relies on later sfence)
```

**Verdict**: ✅ Required.

#### XW.S3 — reservation ring publish flushes (XWS3 value_xfer prep)

L1788-1828: a sequence of flush+fence calls for reservation/ack of the
peer-pool blk_off (for cross-host write where the block lives on owner
host). Each writes CXL ReservationRing entries.

**Verdict**: ✅ Required for cross-host reservation protocol.

#### XW.S4 — staging value publish flushes (XWS3 value_xfer)

```cpp
flush_line((void *)(staging + off)); // L1851, L1864 (per cacheline)
store_fence();                       // L1853, L1866
```

**Cross-host visibility?** Sender writes value bytes to CXL staging /
peer's pool block, receiver reads later.

**Order necessity?** YES — value bytes BEFORE req_op_id publish.

**Verdict**: ✅ Required.

#### XW.S5 — req publish flush + sfence (XWS4 ctrl_publish)

```cpp
flush_line((void *)&e->req_op_id); // L1905
store_fence();                     // L1906
```

**Verdict**: ✅ Required.

### XW receiver — `write_receiver_loop()` + `write_handler()`

#### XW.R1 — ring tail load flush (XWR6 rcv_poll)

```cpp
flush_line((void *)&ring->tail);   // L2754
```

(fence implicit in subsequent acquire load)

**Verdict**: ✅ Required.

#### XW.R2 — per-slot req_op_id flush + mfence (XWR6 rcv_poll)

```cpp
flush_line((void *)&e->req_op_id); // L2764, L2772
full_fence();                      // L2765 (after first)
```

**Verdict**: ✅ Required.

#### XW.R3 — bucket flush + sfence in write_handler (XWR7 rcv_work, currently dummy path)

```cpp
flush_line(&bucket->slots[0]);     // L2502 (write_handler dummy)
store_fence();                     // L2503
```

Active code path: `write_handler` dummy benchmark mode. Not the main
production code which routes through `execute_write_local_with_blk`
(stages LWS1..LWS6 mirror).

**Verdict**: ⚠ Path-dependent; in dummy mode required.

#### XW.R4 — staging value-bytes flush + mfence (XWR7 rcv_work)

```cpp
flush_line((void *)(staging + off)); // L2558
full_fence();                        // L2560
```

Read peer's just-written value from CXL staging.

**Verdict**: ✅ Required.

#### XW.R5 — resp_op_id publish flush + sfence (XWR8 ack_publish)

```cpp
flush_line((void *)&e->resp_op_id);// L2795
store_fence();                     // L2796
```

**Verdict**: ✅ Required.

---

## Path 5 — Other (not on local/xhost RW user-op paths)

These flushes fire on:
- One-time init / attach paths.
- Separate threads (sender aggregator, inval receiver, reservation handler).
- Internal RCU / hazard-pointer book-keeping.
- Dead code (`_v2_unused` variants kept for reference).

### O.1 — Bucket init flush (attach, `cxl_kv_ops_A.cc:433-436`)

One-time per attach: scans buckets and flushes each. **Verdict**: ✅ Required (initialization).

### O.2 — Wire/enable ring init flushes (`cxl_kv_ops_A.cc:299-373`, 895, 945, 2018, 2120, 2132)

One-time ring/staging init. **Verdict**: ✅ Required.

### O.3 — Blockpool init flushes (`cxl_kv_blockpool.cc:91-120`)

One-time pool magic + cursor init for primary and non-primary host.
**Verdict**: ✅ Required.

### O.4 — Aggregator sender-thread CXL ring emits

Active sender threads (per-ring sender, runs on dedicated CPU) drive
the actual CXL ring tail fetch_add + entry publish + flush + sfence.
Workers route through DRAM AggrSlot only (no flush on worker).
Code paths: deeper inside `write_sender_loop` / `read_sender_loop` /
`inval_sender_loop` (separate threads not in user's worker call stack).

**Cross-host visibility?** YES (this is how CXL rings advance).
**Order necessity?** YES.
**Verdict**: ✅ Required.

### O.5 — send_invalidate_direct fallback (`cxl_kv_ops_A.cc:1944-1985`)

Triggered ONLY when aggregator is unavailable (g_aggr_worker_id < 0 OR
worker not registered with aggregator). Default production path uses
aggregator → these flushes don't fire on user ops in the standard
config. Same shape as O.4.

**Verdict**: ✅ Required (when active).

### O.6 — Inval receiver loop (`cxl_kv_ops_A.cc:2202-2236`)

Runs on a dedicated thread; consumes OP_INVALIDATE messages from
peer hosts and updates this host's local cache_pool.

**Cross-host visibility?** Reverse — reads peer-written InvalRing entries.
**Order necessity?** YES — load must re-fetch from CXL.
**Verdict**: ✅ Required.

### O.7 — Reservation handler loop (`cxl_kv_ops_A.cc:2058-2099`)

Owner-host thread that serves peer-host requests for `alloc_peer` block
reservations. Fires only on peer-host writes.

**Verdict**: ✅ Required.

### O.8 — RCU / hazard pointer protect (`cxl_read_guard.h:128-150, 161, 194-200, 208`)

- `rcu_enter` / `rcu_exit` — publishes thread reader_epoch to CXL so a
  retire (rcu_synchronize on peer) waits for in-flight readers.
- `hazard_protect` / `hazard_release` — publishes hazard blk_off so peer's
  retire avoids freeing a block under read.
- `rcu_publish_epoch` — sender-side commit epoch bump.

Called inside `forward_read_direct` (XR sender, lines 2306+) and around
xhost-read receiver staging publish.

**Cross-host visibility?** YES — peers reading reader_epoch /
publish_epoch / hazard_blk_off.
**Order necessity?** YES — strict ordering required for the RCU /
hazard-pointer correctness proof.
**Verdict**: ✅ Required.

### O.9 — Dead code (`_v2_unused` variants, `cxl_kv_ops_A.cc:1175-1542`)

Three V2 variants of the sender-drain logic (`write_sender_drain_dst_v2_unused`,
`read_sender_drain_dst_v2_unused`, `inval_sender_drain_dst_v2_unused`) are
kept as reference but never invoked. The flushes inside are inert at
runtime.

**Verdict**: 🗑 Dead — candidate for deletion.

---

## Master summary table

| Path | Sites with active flush+fence | Removable? | Cross-host needed? | Already isolation-gated | Theoretical fence-saving |
|---|---:|---|---|---|---|
| LR | 2 groups (LR.1, LR.2) | LR.1: ❌ done; LR.2: 🧪 likely | LR.1: no; LR.2: no for owner-self | LR.1 + LR.2 | up to 17 cl + 1 mfence per MISS op |
| LW | 4 groups (LW.1, LW.2, LW.3, LW.4) | LW.1-LW.4: ⚠ all conditional cross-host | YES (peers read these) | LW.1, LW.2/3, LW.4 | up to 18 cl + 2 sfence per blockpool write |
| XR sender | 7 stages (S1..S7) | All ✅ required | YES (RTT protocol) | none | none — required |
| XR receiver | 7 sites (R1..R7) | XR.R3 ❌ removable (owner-self analog) | YES except R3 | none | up to 2 cl + 1 mfence per recv (R3) |
| XW sender | 5 stages (S1..S5) | All ✅ required | YES | none | none |
| XW receiver | 5 sites (R1..R5) | All ✅ required for active prod path | YES | none | none |
| Other | 9 categories (O.1..O.9) | O.9 dead code candidate for deletion; rest required | mixed | none | none |

---

## What this audit changes vs Phase 1's preliminary audit

The preliminary [`iter19A_flush_fence_audit.md`](iter19A_flush_fence_audit.md)
focused on LR + LW only with 6 groups (G1–G6). This v2 extends to:

1. **XR sender** (7 stages, all required) — none removable, this is the
   protocol.
2. **XR receiver** — flagged **XR.R3 (bucket flush on owner host)** as
   the analog of LR.1 / B-H3. Theoretical safe-removable; **not yet
   isolation-gated**.
3. **XW sender + XW receiver** — all flushes required (protocol).
4. **Other** — clarified that dead-code V2 variants exist (candidate
   deletion); init / sender / receiver / aggregator / RCU all required.

**New isolation-candidate emerging from this audit (Task 3c+1)**:

- **XR.R3** "xhost_read receiver bucket flush" — analog of B-H3 on the
  receiver side. Same MOESI argument applies (receiver runs on owner
  host, reads owner's own bucket). Estimated thpt impact unknown until
  measured.

---

## Recommended Task 3c follow-up gates

Beyond the 4 gates already wired in this iter (G2, G3, G45, G6), the
audit surfaces one more candidate:

| Flag (proposed) | Group | Site | Risk | Predicted savings |
|---|---|---|---|---|
| `FUSEE_XR_DEL_RECV_BUCKET_FLUSH` | XR.R3 | `read_handler` bucket scan flush | low (MOESI argument identical to LR.1) | 2 cl + 1 mfence per xhost_read RECV op |

Add to Task 3c sweep if user confirms.
