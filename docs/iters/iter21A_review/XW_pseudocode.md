# XW — Cross-host Write pseudocode (review surface for iter-21A flush removal + STAGING removal)

**Path**: `insert/update/remove(key, …)` falls through to
`forward_write(owner, …)` when `owner_host(key) != self`.  Two halves:
- **XW-req** ([src/cxl_kv_ops_A.cc:1655-1801](../../../src/cxl_kv_ops_A.cc#L1655)
  `forward_write_direct`) — runs on **requester (peer)** host.
- **XW-resp** ([src/cxl_kv_ops_A.cc:2219-2286](../../../src/cxl_kv_ops_A.cc#L2219)
  `write_handler` → dispatches to
  [`execute_write_local_with_blk`:440-521](../../../src/cxl_kv_ops_A.cc#L440)
  or `execute_write_local`) — runs on **owner** host in
  `write_receiver_loop`.

**Update 2026-06-08 (user)**: STAGING fallback for RESERVED-exhaustion
**stays in iter-21A** (deferred to a later iter).  RESERVED is still
the primary XW path; STAGING is the on-exhaustion safety net.  This
means iter-21A keeps the staging code but the production hot path
(when RESERVED has room) avoids it entirely.

---

## XW Part 1 — `forward_write_direct` (peer/requester host)

```
forward_write_direct(owner, key, value, value_len, op_kind):
  if !wr_ or !fs_: return -10
  if value_len > kForwardStagingSlotBytes: return -5

  # ─── [DEAD CODE under default build] self-invalidate local cache ───
  # FUSEE_XHOST_WRITE_SELF_INVAL = 0 by default; entire block compiles out.
  # iter-16A comment: "DEAD CODE in default builds. … Kept as opt-in flag."
  # iter-21A: leave the #if block alone (per LW-Backlog B1 — env knobs not
  # in cleanup scope).

  # ─── Stage 1: ring slot reserve ───
  ring_idx = compute_ring_idx(key)
  ring     = &wr_->rings[host_id_][owner][ring_idx]
  my_op    = write_op_counter_.fetch_add(1)
  op_id    = encode_op_id(host_id_, my_op)

  tpos = ring->tail.fetch_add(1)
  flush_line(&ring->tail)                       # [XW.1]
  store_fence()                                 # [XW.2]
  #   reason: receiver on owner host polls ring->tail. cross-host.
  #   post-partition: KEEP.

  slot_idx = tpos % kWriteRingDepth
  e = &ring->entries[slot_idx]

  # ─── Stage 2: wait for slot free ───
  for ;;:
    flush_line(&e->req_op_id)                   # [XW.3]
    lfence                                      # [XW.4] iter-16A micro-opt
    if e->req_op_id == 0: break
    pause
  #   reason: spin on receiver's clear-to-0. cross-host acquire.
  #   post-partition: KEEP.

  # ─── Stage 3: value transfer — RESERVED direct OR STAGING fallback ───
  direct_blk_off = 0
  direct_used    = false
  if value and value_len>0 and op_kind != DELETE and pool_:
      direct_blk_off = pool_->alloc_peer(owner)  # bump in owner's reserved-for-me sub-segment

  if direct_blk_off != 0:
      # ★ RESERVED direct path ★ — write value bytes to owner's pool segment
      hdr = [vlen as u32]
      pool_->write(direct_blk_off,    hdr,   4)                  # [XW.D1]
      pool_->write(direct_blk_off + 4, value, value_len)         # [XW.D2]
      direct_used = true
      #   pool->write internally: per-cacheline flush_line + sfence.
      #   reason: peer writes into owner's CXL segment; owner's
      #     read_handler / execute_write_local_with_blk will pick up
      #     bytes from this offset. Must push out of peer's L1 to CXL
      #     so owner sees fresh bytes.
      #   post-partition: KEEP (P5 policy — pool->write always flushes).

  if not direct_used:
      # ★★ STAGING fallback ★★ — iter-21A: DELETE THIS BRANCH ★★
      # ([src/cxl_kv_ops_A.cc:1739-1751](../../../src/cxl_kv_ops_A.cc#L1739))
      # Current code path:
      if value and value_len > 0:
          staging = forward_staging_bytes(fs_, host_id_, owner, slot_idx)
          memcpy(staging, value, value_len)
          for off in 0..value_len step 64: flush_line(staging+off)  # [XW.S1]
          store_fence()                                              # [XW.S2]
      #   reason: peer's value bytes go into ForwardStagingMatrix in CXL;
      #     owner's write_handler later memcpy's from there.
      #   iter-21A decision: DELETE the entire if-not-direct_used branch.
      #     RESERVED exhaustion now causes a different recovery (see Open
      #     Question 1 below).

  # ─── Stage 4: publish control message ───
  e->key       = key
  e->op_kind   = op_kind
  e->value_len = value_len
  if direct_used:
      e->staging_off = direct_blk_off            # owner reads this as blk_off
      e->staging_gen = 0
  else:
      e->staging_off = slot_idx                  # owner reads this as slot index → STAGING path
      e->staging_gen = 1
  # iter-21A: with STAGING removed, staging_gen is always 0.  Could either:
  #   (a) drop the staging_gen field entirely (rename staging_off → blk_off)
  #   (b) keep field, always set to 0, write_handler asserts on != 0
  # See Decision XW-D2 below.

  e->resp_op_id = 0
  e->status     = 0
  thread_fence(release)
  e->req_op_id.store(op_id, release)
  flush_line(&e->req_op_id)                     # [XW.5]
  store_fence()                                 # [XW.6]
  #   reason: receiver picks up via req_op_id != 0. cross-host publish.
  #   post-partition: KEEP.

  # ─── Stage 5: ack wait ───
  rc = generic_spin_wait(e, op_id, &status)
  #   internally: flush+lfence on resp_op_id cacheline, cross-host. KEEP.

  return (rc != 0) ? rc : status
```

---

## XW Part 2 — `write_handler` (owner host, in write_receiver_loop)

```
write_handler(e, src):
  nlevel = recv_noop_level()

  # ─── L3 NOOP: pure ack ───
  if nlevel >= 3:
      e->status = 0; return                      # No CXL access, no flush

  # ─── L2 NOOP: dummy slot publish only ───
  # bench-only scaffolding (XW-D5). Iter-21A leaves it alone.
  if nlevel >= 2:
      if op_kind != DELETE and staging_gen == 0 and buckets_:
          b      = bucket_idx(e->key)
          bucket = &buckets_[b]
          encoded = pack(e->staging_off, kSizeClassBlock256, fingerprint(e->key))
          bucket->slots[0].value = encoded
          flush_line(&bucket->slots[0])         # [XW.7]
          store_fence()                          # [XW.8]
          # bench code measuring slot publish cost; bucket access is
          # owner-self post-partition → MOESI safe; flush is conceptually
          # vestigial but bench code stays per policy (XR-D4-style).
      e->status = 0; return

  # ─── L0/L1: production path ───
  if op_kind == DELETE:
      e->status = execute_write_local(e->key, nullptr, 0, DELETE, src)
      return                                     # → LW path; no XW-specific flushes

  if value_len == 0 or value_len > kForwardStagingSlotBytes:
      e->status = -5; return

  # iter-21A: with STAGING fallback removed, staging_gen MUST be 0.
  # Replace the branch below with an assert or unconditional dispatch.
  if e->staging_gen == 0:
      # ★ RESERVED direct path ★ — owner just publishes the pre-written blk_off
      blk_off  = e->staging_off
      e->status = execute_write_local_with_blk(e->key, blk_off, value_len, op_kind, src)
      return                                     # → see Part 3 below

  # ★★ STAGING fallback path ★★ — iter-21A: DELETE THIS ENTIRE TAIL ★★
  # ([src/cxl_kv_ops_A.cc:2274-2285](../../../src/cxl_kv_ops_A.cc#L2274))
  # Current code:
  slot_idx = e->staging_off
  staging  = forward_staging_bytes(fs_, src, host_id_, slot_idx)
  for off in 0..value_len step 64:
      flush_line(staging + off)                  # [XW.S3]
  full_fence()                                   # [XW.S4]
  #   reason: owner reads peer's staging from CXL; must flush local L1 first.
  #   iter-21A: gone with STAGING removal.
  e->status = execute_write_local(e->key, staging, value_len, op_kind, src)
```

---

## XW Part 3 — `execute_write_local_with_blk` (owner host, RESERVED path)

Called from `write_handler` with `blk_off` already populated by peer
into peer's reserved sub-region of owner's pool.  Owner does NOT
alloc, NOT memcpy value bytes — just publishes the slot pointer.

```
execute_write_local_with_blk(key, blk_off, value_len, op_kind, self_inval_src):
  # bucket scan + directory lock — same as LW Step 0-2
  b      = bucket_idx(key)                       # post-partition: in owner segment
  bucket = &buckets_[b]
  # iter-17A: bucket pre-scan flush already removed (line 454-460 comment).

  for s in 0..6:
      if slot[s].key == key:         match = s; break
      elif slot[s].key == kEmptyKey and empty<0: empty = s
  # INSERT/UPDATE branching (DELETE doesn't reach this function; falls
  # back to execute_write_local).

  de = slot_directory_entry(dir_, b, target_slot)
  slot_directory_lock(de)

  # ─── Sharer invalidate (same as LW Step 3) ───
  bitmap = de->sharer_bitmap
  if recv_noop_level() < 1 and op_kind != INSERT and num_hosts_ > 1 and ir_:
      for h in 0..num_hosts_:
          if h == host_id_: continue
          if h == self_inval_src: continue       # FUSEE_XHOST_WRITE_SELF_INVAL gate
          if not (bitmap & (1 << h)): continue
          send_invalidate(h, key)                # [XW.X1] cross-host InvalRing — KEEP

  # ─── CoW publish (worker pre-allocated blk_off) ───
  encoded = cxl_slot_pack(blk_off, kSizeClassBlock256, key_fingerprint(key))
  publish_slot_cow(slot, key, encoded)           # [XW.9 / XW.10] same site as LW.B5/B6
                                                 # → already covered by LW-D3 (REMOVE)

  # ─── Directory state ───
  de->version++
  de->state = kDirStateShared
  de->sharer_bitmap = (1 << host_id_)
  slot_directory_unlock(de)

  # ─── Cache evict (no insert — owner doesn't have value bytes) ───
  cache_pool_evict(cache_, key)                  # DRAM
  return 0
```

---

## Summary — XW partition + STAGING-removal impact

### XW-req (peer/requester side)

| tag | site | flush | pre-partition reason | post-partition + iter-21A decision |
|---|---|---|---|---|
| `[XW.1]/[2]` | 1690-1692 `&ring->tail` | flush+sfence | cross-host: receiver polls | **KEEP** |
| `[XW.3]/[4]` | 1705-1706 `&e->req_op_id` | flush+lfence | cross-host spin acquire | **KEEP** |
| `[XW.D1]/[D2]` | 1735-1736 `pool_->write(direct_blk_off, …)` | per-cacheline flush+sfence (internal) | peer writes to owner's segment; owner must see fresh | **KEEP** (P5) |
| `[XW.S1]/[S2]` | 1746-1749 STAGING staging-bytes flush+sfence | flush+sfence on peer's staging matrix | (peer pushes value bytes into ForwardStagingMatrix on CXL for owner pickup) | **REMOVE entire branch** (lines 1739-1751) |
| `[XW.5]/[6]` | 1783-1784 `&e->req_op_id` publish | flush+sfence | cross-host publish | **KEEP** |
| (generic_spin_wait sites) | 332-353 | various | cross-host ack spin | **KEEP** |

### XW-resp (owner side, in write_handler / execute_write_local_with_blk)

| tag | site | flush | pre-partition reason | post-partition + iter-21A decision |
|---|---|---|---|---|
| `[XW.7]/[8]` | 2239-2240 L2 NOOP slot flush+sfence | bench scaffolding | L2 bucket-touch attribution | **KEEP** (bench-only, per XR-D4 policy) |
| `[XW.S3]/[S4]` | 2281-2283 STAGING fallback staging-bytes flush+fence | owner reads peer's staging from CXL | (owner copies value bytes from ForwardStagingMatrix into its own pool) | **REMOVE entire tail** (lines 2274-2285) |
| `[XW.X1]` | execute_write_local_with_blk line 498 send_invalidate | per-peer call | cross-host InvalRing | **KEEP** (same as LW-D1) |
| `[XW.9]/[10]` | 506 publish_slot_cow | flush+sfence on slot | peer scan/cache-fetch on owner's bucket (pre-partition) | **REMOVE** (already covered by LW-D3 — same source-edit) |

(`pool->write` flush inside `forward_write_direct` and the implicit
flush inside `pool->write` of execute_write_local arms are tagged
elsewhere; they remain KEEP per P5.)

---

## Final decisions (locked 2026-06-08, source of truth for implementation)

### Decision XW-D1: STAGING fallback path — KEEP (deferred)

**User update 2026-06-08**: STAGING-on-RESERVED-exhaustion stays in
iter-21A.  Both branches survive:

- Peer side ([src/cxl_kv_ops_A.cc:1731-1751](../../../src/cxl_kv_ops_A.cc#L1731)):
  if `pool_->alloc_peer(owner) != 0` → RESERVED direct (`[XW.D1]`/`[XW.D2]`);
  else → STAGING fallback (`[XW.S1]`/`[XW.S2]`).  Both paths KEEP.
- Owner side ([src/cxl_kv_ops_A.cc:2268-2285](../../../src/cxl_kv_ops_A.cc#L2268)):
  `staging_gen == 0` → `execute_write_local_with_blk`;
  `staging_gen == 1` → STAGING tail (`[XW.S3]`/`[XW.S4]`).  Both paths
  KEEP.

`ForwardStagingMatrix` data structure + `forward_staging_bytes()` +
`forward_staging_matrix_bytes()` STAY.  `e->staging_gen` field stays
as a runtime mode marker.

This is the ONLY substantive deviation between iter-21A and a clean
"only HAZARD/RESERVED" architecture — the deviation is sanctioned by
user as a deferred follow-up.

### Decision XW-D2: WriteEntry `staging_gen` field — keep as-is

Per XW-D1, both modes coexist.  `staging_gen ∈ {0, 1}` is a real
runtime flag, not a vestige.  No rename, no field changes in iter-21A.

### Decision XW-D3: RESERVED exhaustion semantics — status quo

`pool_->alloc_peer(owner) == 0` → STAGING fallback (unchanged).
Open Question 1 below is RESOLVED by user 2026-06-08: keep STAGING.

### Decision XW-D4: All cross-host ring/staging coord flushes — KEEP

`[XW.1/2/3/4/5/6]` on peer side + the generic_spin_wait flushes +
send_invalidate cross-host coord internals — all KEEP, partition
doesn't affect them.

### Decision XW-D5: NOOP branches preserved

L2 / L3 `recv_noop_level()` branches stay, including `[XW.7]/[XW.8]`
L2 slot-flush (bench attribution, gated by env, not production).

### Decision XW-D6: bucket / slot flush removal already covered by LW

`[XW.9]/[XW.10]` is the same `publish_slot_cow` source as
LW-D3.  One edit there covers both paths.  iter-17A already cleaned
the bucket pre-scan flush in `execute_write_local_with_blk`
([src/cxl_kv_ops_A.cc:454-460](../../../src/cxl_kv_ops_A.cc#L454)
comment).  No additional XW-side bucket edits needed.

---

## Open questions specifically about XW

### 1. RESERVED exhaustion recovery — RESOLVED 2026-06-08

User decision: **STAGING fallback stays in iter-21A**.  Status quo.
`pool_->alloc_peer == 0` → peer falls back to copying value bytes into
ForwardStagingMatrix; owner's write_handler `staging_gen == 1` branch
picks them up and runs `execute_write_local` (the STAGING-copy path).

Full STAGING removal will land in a future iter together with
RESERVED reclaim (Backlog B3 below).

### 2. `FUSEE_XHOST_WRITE_SELF_INVAL` — stays per Backlog B1

Current default 0 means: peer does NOT self-invalidate before
forwarding; receiver does NOT skip src in broadcast.  Both sides
match (correct, just with an extra cross-host RTT).  iter-21A leaves
this alone per user policy on LW env knobs.

### 3. `recv_noop_level()` bench branches

Same as XR Open Question 2 — keep as bench scaffolding, not
production toggle.

---

## Backlog (not iter-21A scope)

### B1. Rename `ForwardStagingMatrix` family + related fields

Same rename treatment as XR-Backlog B1 (concrete naming TBD,
`hazard`-themed).  Deferred since XW-D1 keeps the structure alive in
iter-21A.

### B2. STAGING fallback elimination + RESERVED reclaim

Two coupled pieces to land together in a future iter:
1. Wire a freelist/hazard-pointer-based reclaim path so RESERVED
   sub-segments can free blocks back to the peer-reserved pool.
2. With reclaim in place, STAGING fallback can be retired entirely
   (exhaustion either spin-waits on reclaim or returns an error).

Once that lands, `ForwardStagingMatrix` + `forward_staging_bytes()` +
`staging_gen` / `staging_off` WriteEntry fields can all be deleted +
renamed in one cleanup pass.

iter-21A does NOT pre-size RESERVED specially for the benchmark — we
let the existing `peer_blocks_per_peer_` default handle the workload,
and if benchmark cells trip exhaustion we let STAGING absorb it (with
some perf hit but no correctness loss).