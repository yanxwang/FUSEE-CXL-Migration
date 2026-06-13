# LW — Local Write pseudocode (review surface for iter-21A flush removal)

**Path**: `insert/update/remove(key, …)` when `owner_host(key) == self`,
**OR** owner's `write_handler` calling `execute_write_local(…, src=peer)`
in the receiver loop to process a forwarded write.

**Source**: [src/cxl_kv_ops_A.cc:529-724](../../../src/cxl_kv_ops_A.cc#L529)
`CxlKvStoreA::execute_write_local` + the two inline helpers
[publish_slot_cow:284-292](../../../src/cxl_kv_ops_A.cc#L284) and
[retire_slot:294-300](../../../src/cxl_kv_ops_A.cc#L294) +
[CxlKvBlockPool::write:170-189](../../../src/cxl_kv_blockpool.cc#L170).

---

## LW pseudocode

```
execute_write_local(key, value, value_len, op_kind, self_inval_src=-1):

  # ─── Step 0: validate + bucket lookup ───
  if key == kEmptyKey: return -1
  if op_kind != DELETE: validate value/value_len; check pool block_size
  b      = bucket_idx(key)                          # post-partition: in owner segment
  bucket = &buckets_[b]
  #   iter-17A already removed pre-scan bucket flush+mfence
  #   ([src/cxl_kv_ops_A.cc:546-548](../../../src/cxl_kv_ops_A.cc#L546)
  #    comment: "x86 MOESI keeps owner's L1 coherent without explicit clflushopt").
  #   iter-21A doc-update: change that comment from "iter-17A removed" to
  #   "no flush needed (owner-self only after iter-21A partition)".

  # ─── Step 1: scan bucket for match / empty ───
  match = -1; empty = -1
  for s in 0..6:                                    # kCxlKvSlotsPerBucket=7
    if slot[s].key == key:         match = s; break
    elif slot[s].key == kEmptyKey and empty<0: empty = s

  if INSERT and match >= 0:   return -2 (dup)
  if INSERT and empty < 0:    return -3 (full)
  if UPDATE/DELETE and match<0: return -1 (not found)
  target_slot = (INSERT ? empty : match)

  # ─── Step 2: lock directory entry (DRAM) ───
  de = slot_directory_entry(dir_, b, target_slot)
  slot_directory_lock(de)                           # pthread_spinlock host-local
                                                    # DRAM, no flush around it
  #   iter-17A already removed post-lock bucket flush
  #   ([src/cxl_kv_ops_A.cc:576](../../../src/cxl_kv_ops_A.cc#L576) comment).

  slot = &bucket->slots[target_slot]

  # ─── Step 3: broadcast OP_INVALIDATE to peer sharers \ {self} ───
  bitmap = de->sharer_bitmap
  if op_kind != INSERT and num_hosts_ > 1 and ir_:
    for h in 0..num_hosts_:
      if h == self_inval_src: continue              # FUSEE_XHOST_WRITE_SELF_INVAL gate
                                                    # — see Backlog B1
      if bitmap & (1 << h) == 0: continue
      send_invalidate(h, key)                       # [LW.X1] → send_invalidate_direct
                                                    #   internal: cross-host InvalRing
                                                    #   flush+sfence (multiple sites)
                                                    #   post-partition: KEEP (cross-host
                                                    #   coordination — unchanged)

  # ─── Step 4: CoW publish ───
  if op_kind == DELETE:
      retire_slot(slot):
        slot->key = kEmptyKey (atomic release)
        flush_line(slot)                            # [LW.B1]
        store_fence()                               # [LW.B2]
        #   pre-partition reason: peer scan/insert/cache-fetch on this same
        #     bucket would otherwise see stale (non-empty) slot.
        #   post-partition: ★ REMOVE both ★
        #     - peer never reads/writes owner's bucket
        #     - owner-self consumers (search / execute_write_local /
        #       read_handler on owner host) MOESI-coherent

  elif pool_ == nullptr:                            # inline-u64 fallback
      inline_v = pack(value into u64)
      publish_slot_cow(slot, key, inline_v):
        slot->value = inline_v
        slot->key   = key
        flush_line(slot)                            # [LW.B3]
        store_fence()                               # [LW.B4]
        # same as [LW.B1/B2] — REMOVE both

  else:                                             # full blockpool CoW
      blk_off = pool_->alloc()                      # bump per-host cursor
      if blk_off == 0:
        slot_directory_unlock(de); return -4

      # 4-B header (value_len) + value bytes
      buf = [vlen as u32] ++ value_bytes
      pool_->write(blk_off, buf, 4+value_len)       # [LW.D1] → per cacheline
                                                    #   flush_line + sfence (CxlKvBlockPool::write)
                                                    #   reason: peer later does read_xhost on
                                                    #     this block. bytes must be durable in CXL.
                                                    #   post-partition: KEEP (P5 policy —
                                                    #   pool->write always flushes regardless
                                                    #   of bucket partition).

      encoded = cxl_slot_pack(blk_off, size_class, fingerprint)
      publish_slot_cow(slot, key, encoded):
        slot->value = encoded
        slot->key   = key
        flush_line(slot)                            # [LW.B5]
        store_fence()                               # [LW.B6]
        # same site as [LW.B3/B4] (publish_slot_cow is an inline function called
        # from two arms of the if/else) — REMOVE both
        # peer never reads owner's slot directly; XR path goes through
        # read_handler on owner → MOESI-coherent.

  # ─── Step 5: update directory state (DRAM) ───
  de->version++
  if op_kind == DELETE:
      de->state = kDirStateInvalid
      de->sharer_bitmap = 0
  else:
      de->state = kDirStateShared
      de->sharer_bitmap = (1 << host_id_)           # self only; peers re-register

  slot_directory_unlock(de)                         # DRAM spinlock release

  # ─── Step 6: own cache update (DRAM) ───
  if op_kind == DELETE:
      cache_pool_evict(cache_, key)                  # bump bucket epoch
  else:
      cache_pool_insert(cache_, key, value, value_len)
  # cache_pool is DRAM, seqlock CAS, no CXL flush

  return 0
```

---

## Final decisions (locked 2026-06-08, source of truth for implementation)

### Decision LW-D1: `[LW.X1]` send_invalidate — KEEP

[src/cxl_kv_ops_A.cc:608](../../../src/cxl_kv_ops_A.cc#L608) call site
goes into `send_invalidate_direct` ([src/cxl_kv_ops_A.cc:1812-1864](../../../src/cxl_kv_ops_A.cc#L1812))
which has 10 internal flush+sfence sites — all cross-host InvalRing
coordination, all KEEP unchanged by bucket partition.

(The send_invalidate internal flushes don't get individual `[LW.X1.…]`
tags in this doc; they're part of the cross-host coord pool covered
under policy P4.)

### Decision LW-D2: `[LW.B1] / [LW.B2]` retire_slot — REMOVE

[src/cxl_kv_ops_A.cc:297-298](../../../src/cxl_kv_ops_A.cc#L297)
inside `retire_slot`.  Delete the 2 lines (`flush_line(slot)` +
`store_fence()`) and the surrounding `#if !FUSEE_LW_DEL_RETIRE_FLUSH`
wrapper.

Same rationale as LR-D1 / XR-D2: bucket partition makes the slot
owner-exclusive; same-host MOESI keeps L1 coherent.

### Decision LW-D3: `[LW.B3] / [LW.B4] / [LW.B5] / [LW.B6]` publish_slot_cow — REMOVE

[src/cxl_kv_ops_A.cc:289-290](../../../src/cxl_kv_ops_A.cc#L289)
inside `publish_slot_cow` (called from both inline-fallback and
blockpool arms).  Delete the 2 lines + the surrounding
`#if !FUSEE_LW_DEL_SLOT_PUB_FLUSH` wrapper.  One source edit, both
call-site tags satisfied.

### Decision LW-D4: `[LW.D1]` pool_->write internal flush — KEEP

[src/cxl_kv_blockpool.cc:177-188](../../../src/cxl_kv_blockpool.cc#L177)
inside `CxlKvBlockPool::write`.  No change to the flush body.

Per policy P5 (pool->write always flushes regardless of who's
writing) the body stays.  The `#if !FUSEE_LW_DEL_POOL_WRITE_FLUSH`
wrapper sits around this — see Backlog B1 (CMake plumbing left alone
in iter-21A).

### Decision LW-D5: Doc-comment update

iter-17A's "removed bucket flush" comments at
[src/cxl_kv_ops_A.cc:546-548](../../../src/cxl_kv_ops_A.cc#L546) and
[576](../../../src/cxl_kv_ops_A.cc#L576) get re-worded to
"no flush needed (owner-self only after iter-21A partition)" so future
readers don't think the flush is missing by oversight.

---

## Backlog (not iter-21A scope)

### B1. Five env-knob toggles on LW path — defer cleanup

Per user policy 2026-06-08: the "no toggle switches on production
paths" rule applies specifically to **XR protocol-mode toggles**
(STAGING / RCU → keep HAZARD) and **XW protocol-mode toggles**
(STAGING / BATCHED → keep RESERVED).  Other env knobs along LW —
the ones that gate flush behavior or auxiliary path behavior — are
NOT in scope for iter-21A cleanup.

The 5 LW env knobs that remain unchanged in iter-21A:

| Env knob | Where | What it gates | iter-21A action |
|---|---|---|---|
| `FUSEE_LW_DEL_SLOT_PUB_FLUSH` | `publish_slot_cow` ([src/cxl_kv_ops_A.cc:288](../../../src/cxl_kv_ops_A.cc#L288)) | The `flush_line(slot) + store_fence()` after CoW publish.  Iter-19A ablation knob. | iter-21A removes the **flush body** (per LW-D3) but **leaves the env-knob plumbing alone** (the `#if` wrapper around an empty body is fine; CMake `-D` plumbing untouched).  Backlog: clean up CMake when env knob is retired. |
| `FUSEE_LW_DEL_RETIRE_FLUSH` | `retire_slot` ([src/cxl_kv_ops_A.cc:296](../../../src/cxl_kv_ops_A.cc#L296)) | Same as above, for DELETE's slot clear.  iter-19A ablation knob. | Same treatment as `_SLOT_PUB_FLUSH`. |
| `FUSEE_LW_DEL_POOL_WRITE_FLUSH` | `CxlKvBlockPool::write` ([src/cxl_kv_blockpool.cc:177](../../../src/cxl_kv_blockpool.cc#L177)) | The per-cacheline flush + sfence inside pool->write.  iter-19A ablation knob. | iter-21A keeps the flush body (per LW-D4); leaves the env-knob plumbing alone. |
| `FUSEE_DISABLE_CACHE_POOL` | `cache_pool_evict/insert` calls in LW Step 6 (lines [710](../../../src/cxl_kv_ops_A.cc#L710), [714](../../../src/cxl_kv_ops_A.cc#L714)) | Disables the entire DRAM cache.  Used to run cache=off ablation builds. | iter-21A leaves alone.  The cache stays default-on in production. |
| `FUSEE_XHOST_WRITE_SELF_INVAL` | execute_write_local's invalidate broadcast loop ([src/cxl_kv_ops_A.cc:598-601](../../../src/cxl_kv_ops_A.cc#L598)) | Skips the host that forwarded this write (already self-invalidated before sending).  iter-14A perf optimization. | iter-21A leaves alone. |

All 5 stay until a future cleanup iter explicitly retires them.  None
of them block iter-21A's bucket-partition + flush-removal work, and
their CMake `-D` plumbing produces no code when unset.

### B2. Rename of `ForwardStagingMatrix` etc. on the XW side

Same rationale as XR-Backlog B1 (rename to a `hazard`-themed
identifier deferred — concrete naming TBD).  Will be covered in the
XW pseudocode doc's Backlog section.