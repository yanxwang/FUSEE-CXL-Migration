# XR — Cross-host Read pseudocode (review surface for iter-21A flush removal)

**Path**: `search(key, …)` falls through to `forward_read(owner, …)` when
`owner_host(key) != self`.  Two halves:
- **XR-req** ([src/cxl_kv_ops_A.cc:2052-2217](../../../src/cxl_kv_ops_A.cc#L2052)
  `forward_read_direct`) — runs on **requester (peer)** host.
- **XR-resp** ([src/cxl_kv_ops_A.cc:~2300-2400](../../../src/cxl_kv_ops_A.cc#L2300)
  `read_handler`) — runs on **owner** host; receives ring requests in
  `read_receiver_loop` and dispatches.

Both halves are entered while `owner != self` (set in caller).  Per
iter-20A §2 staging-removal decision (CONFIRMED for iter-21A scope),
the XR protocol is **HAZARD direct-pool-read** — owner publishes only
`blk_off` in the staging slot; peer fetches value bytes directly from
owner's pool segment.

**Annotation key** (same as LR doc):
- `[XR.…]` tag for review reference
- type tag in brackets: `B` = bucket cacheline, `D` = pool data, `R`
  = ring/staging cross-host coord, `M` = same-host directory

---

## XR-req: forward_read_direct (peer/requester side)

```
forward_read_direct(owner, key, out_buf, buf_len, *out_len):
  ring_idx = compute_ring_idx(key)              # iter-17A Plan A/B routing
  ring = &rr_->rings[host_id_][owner][ring_idx]
  my_op = read_op_counter_.fetch_add(1)
  op_id = encode_op_id(host_id_, my_op)

  # ─── Stage 1: slot_reserve ───
  tpos = ring->tail.fetch_add(1)                # CXL atomic on ring tail
  flush_line(&ring->tail)                       # [XR.R1]  
  store_fence()                                 # [XR.R2]
  #   reason: peer's read_receiver_loop polls tail to know how many entries
  #     to scan. Must flush so receiver sees the new tail. CROSS-HOST coord.
  #   post-partition: KEEP (ring entries are by definition cross-host).

  slot_idx = tpos % kReadRingDepth
  e = &ring->entries[slot_idx]

  # ─── Stage 2: slot_wait ───
  for ;;:                                       # wait for prior occupant to free
    flush_line(&e->req_op_id)                   # [XR.R3]
    full_fence()                                # [XR.R4]
    #   reason: looking at remote cacheline (peer/owner cleared
    #     req_op_id=0 to mark slot free). CROSS-HOST acquire.
    #   post-partition: KEEP.
    if e->req_op_id.load(acquire) == 0: break

  my_epoch_at_send = cache_pool_bucket_epoch(cache_, key)   # DRAM, no flush

  # ─── Stage 3: req_publish ───
  st = read_staging_slot(rs_, host_id_, owner, ring_idx, slot_idx)
  st->ready_op_id.store(0, release)              # clear staging signal
  flush_line(&st->ready_op_id)                  # [XR.R5]  (single sfence at end of Stage 3, see [XR.R8])

  e->key = key
  e->resp_op_id = 0; e->status = 0
  e->resp_value_len = 0; e->resp_blk_off = 0
  thread_fence(release)
  e->req_op_id.store(op_id, release)            # publish request
  flush_line(&e->req_op_id)                     # [XR.R6]
  store_fence()                                 # [XR.R7] + covers [XR.R5] via iter-18A C8 combine
  #   reason: receiver picks up via req_op_id != 0. CROSS-HOST publish.
  #   post-partition: KEEP all of [XR.R5/R6/R7].

  # ─── Stage 4: ack_wait ───  
  t0 = now_ns()
  for ;;:
    flush_line(&st->ready_op_id)                # [XR.R9]
    full_fence()                                # [XR.R10]
    if st->ready_op_id.load(acquire) == op_id: break
    if timeout: break
    pause × 4
  #   reason: owner's read_handler publishes ready_op_id. CROSS-HOST acquire.
  #   post-partition: KEEP.

  # ─── Stage 5: post_ack cleanup ───
  e->req_op_id.store(0, release)                # free ring slot
  flush_line(&e->req_op_id)                     # [XR.R11]
  store_fence()                                 # [XR.R12]
  #   reason: next user of this slot's slot_wait spins on req_op_id==0.
  #   post-partition: KEEP.

  # ─── Stage 5b: validate + branch ───
  # iter-18A C5: no flush needed here — Stage 4's flush already brought
  # the staging control cacheline (incl. lookup_epoch, status, value_size)
  # to fresh CXL state.
  if st->lookup_epoch < my_epoch_at_send:
      return -3                                  # stale snapshot
  if st->status != 0: return st->status
  vlen = st->value_size
  if vlen == 0: return 0

  # ─── Stage 6: value_recv (HAZARD direct-pool-read) ───
  blk_off = st->resp_blk_off                     # owner published blk_off here
  hazard_protect(haz_, host_id_, tid, blk_off)   # publish hazard ptr BEFORE read
  if blk_off == 0 or pool_ == nullptr:
      # Inline-8B fallback: value packed into st->resp_blk_off itself
      memcpy(out_buf, &st->resp_blk_off, copy_len)
  else:
      # ★★★ THE peer-reads-owner's-pool moment ★★★
      pool_->read(blk_off + 4, out_buf, copy_len)     # [XR.D1]  
      #   pre-partition: pool->read internally does flush_line per cacheline
      #     + full_fence — this is the [LR.P1]/[LR.P2] pair from LR doc.
      #     CORRECT for this site because peer is reading owner's CXL
      #     segment; without flush, peer's L1 may serve stale.
      #   post-partition + LR-D2: switch to pool_->read_xhost(...)
      #     to keep the flush explicit-by-direction. (Owner-self callers
      #     use read_local; cross-host XR uses read_xhost.)
  hazard_release(haz_, host_id_, tid)
  *out_len = vlen
  return 0
```

---

## XR-resp: read_handler (owner side, runs in read_receiver_loop)

```
read_handler(e, src):                              # e = ReadEntry from peer
  b      = bucket_idx(e->key)                      # post-partition: in owner segment by construction
  bucket = &buckets_[b]
  ring_idx = ...; slot_idx = ...                   # derive routing for st

  # ─── iter-16A Receiver-NOOP study branches (L3/L2) ───
  # These are benchmark-only ablation paths; not the hot path in
  # production runs (nlevel=0 by default).  Iter-21A leaves them
  # alone unless the user wants them retired.

  if nlevel >= 3:                                  # pure ack
      st_noop = read_staging_slot(rs_, src, host_id_, ring_idx, slot_idx)
      st_noop->key = e->key; ...                   # clear staging fields
      flush_line(st_noop)                          # [XR.R13]
      store_fence()                                # [XR.R14]
      st_noop->ready_op_id.store(req_op_id, release)
      flush_line(&st_noop->ready_op_id)            # [XR.R15]
      store_fence()                                # [XR.R16]
      e->status = 0; return                        #   CROSS-HOST publish — KEEP

  if nlevel >= 2:                                  # touch bucket but skip pool
      flush_line(bucket)                           # [XR.B1] L2-NOOP bucket flush
      full_fence()                                 # [XR.B2]
      #   pre-partition reason: peer might have INSERTed into a slot in
      #     this bucket; need fresh cacheline to measure realistic cost.
      #   post-partition: ★ REMOVE ★ (owner-self bucket access after partition)
      (void)bucket->slots[0].value
      st_noop = read_staging_slot(...)
      flush_line(st_noop); store_fence()           # [XR.R17/R18]   KEEP
      st_noop->ready_op_id.store(req_op_id, release)
      flush_line(&st_noop->ready_op_id); store_fence()  # [XR.R19/R20]  KEEP
      e->status = 0; return

  # ─── L0/L1: full path ───
  flush_line(bucket)                               # [XR.B3] owner-self bucket pre-scan flush
  flush_line(bucket + 64)                          # [XR.B4]
  full_fence()                                     # [XR.B5]
  #   pre-partition reason: peer might have INSERTed into bucket; receiver
  #     (running on owner host) needs fresh cacheline before scan.
  #   post-partition: ★ REMOVE all three ★ (owner-self bucket; same-host
  #     MOESI on owner's L1 covers any concurrent same-host worker writes
  #     to this bucket. Peer never writes to owner's bucket after partition.)

  lookup_epoch = cache_pool_bucket_epoch(cache_, e->key)
  st = read_staging_slot(rs_, src, host_id_, ring_idx, slot_idx)

  # publish_staging lambda (called below from miss / hit / inline / pool branches)
  publish_staging(st_status, vlen):
      st->key = e->key
      st->value_size = vlen
      st->status = st_status
      st->lookup_epoch = lookup_epoch
      flush_line(st)                               # [XR.R21]
      store_fence()                                # [XR.R22]
      #   reason: staging control fields must be visible to peer BEFORE
      #     ready_op_id flip — peer's Stage 5b reads lookup_epoch / status /
      #     vlen after seeing ready_op_id.  CROSS-HOST publish.
      #   post-partition: KEEP.
      st->ready_op_id.store(req_op_id, release)
      flush_line(&st->ready_op_id)                 # [XR.R23]
      store_fence()                                # [XR.R24]
      #   reason: peer spins on ready_op_id. CROSS-HOST release.
      #   post-partition: KEEP.

  # Scan bucket for the key
  found = -1
  for s in 0..6:
      if bucket->slots[s].key == e->key: found = s; break
  if found < 0:
      publish_staging(-1, 0)
      e->status = -1; return

  # Lock directory entry to update sharer_bitmap
  de = slot_directory_entry(dir_, b, found)
  slot_directory_lock(de)                          # DRAM pthread_spinlock — same-host only
  if src != host_id_: de->sharer_bitmap |= (1 << src)
  encoded = bucket->slots[found].value
  slot_directory_unlock(de)
  #   directory lock is host-local DRAM, no flush around it.

  # Branch on size class
  if size_class == kSizeClassInline or pool_ == nullptr:
      # Pack value into st->resp_blk_off (high bit signals inline-8B)
      st->resp_blk_off = encoded
      publish_staging(0, vlen=8)
  else:
      # HAZARD: publish blk_off only, peer fetches via read_xhost
      blk_off = slot_blk_off(encoded)
      hdr_buf[4] = ...
      pool_->read(blk_off, hdr_buf, 4)             # [XR.D2] receiver reads own pool
      vlen = parse_u32(hdr_buf)
      #   pre-partition: pool->read internally flushes (LR.P1/P2).
      #     CORRECT — but the flush is overkill for owner-self.
      #   post-partition + LR-D2: switch to pool_->read_local(blk_off, hdr_buf, 4)
      #     — receiver IS on owner host reading owner's own segment, MOESI safe.
      st->resp_blk_off = blk_off                   # peer will read via XR.D1
      publish_staging(0, vlen)
  e->status = 0
  return
```

---

## Summary — XR partition impact

### XR-req (peer/requester side)

| tag | site | flush | type | pre-partition reason | post-partition decision |
|---|---|---|---|---|---|
| `[XR.R1]/[R2]` | 2066-2067 | `flush+sfence(&ring->tail)` | R | cross-host: receiver polls tail | **KEEP** |
| `[XR.R3]/[R4]` | 2076-2077 | `flush+mfence(&e->req_op_id)` | R | cross-host: spin on slot free | **KEEP** |
| `[XR.R5]` | 2102 | `flush(&st->ready_op_id)` | R | cross-host: clear signal | **KEEP** (sfence combined with R7) |
| `[XR.R6]/[R7]` | 2111-2112 | `flush+sfence(&e->req_op_id)` | R | cross-host: publish request | **KEEP** |
| `[XR.R9]/[R10]` | 2126-2127 | `flush+mfence(&st->ready_op_id)` | R | cross-host: spin on ack | **KEEP** |
| `[XR.R11]/[R12]` | 2149-2150 | `flush+sfence(&e->req_op_id=0)` | R | cross-host: free slot | **KEEP** |
| `[XR.D1]` | 2209 | `pool_->read(...)` (internally [LR.P1]/[LR.P2]) | D | peer reads owner's pool segment | **SWITCH** to `pool_->read_xhost(...)` per LR-D2 |

### XR-resp (owner side, in read_handler)

| tag | site | flush | type | pre-partition reason | post-partition decision |
|---|---|---|---|---|---|
| `[XR.B1]/[B2]` | 2330-2331 | `flush+mfence(bucket)` (L2 NOOP) | B | bench-mode bucket touch | **REMOVE** (owner-self bucket) |
| `[XR.B3]/[B4]/[B5]` | 2349-2351 | `flush(bucket)+flush(+64)+mfence` (L0/L1) | B | peer-write visibility | **REMOVE** (owner-self bucket; partition) |
| `[XR.D2]` | implicit in `pool_->read(blk_off, hdr_buf, 4)` | pool->read internal flush | D | overkill for owner-self | **SWITCH** to `pool_->read_local(...)` per LR-D2 |
| `[XR.R13]–[R20]` | 2319-2342 | various staging+ready_op_id flushes (L3/L2 NOOP) | R | cross-host: peer reads staging | **KEEP** |
| `[XR.R21]/[R22]` | 2368-2369 | `flush+sfence(st)` (publish_staging) | R | cross-host: staging visibility | **KEEP** |
| `[XR.R23]/[R24]` | 2373-2374 | `flush+sfence(&st->ready_op_id)` | R | cross-host: ack release | **KEEP** |

---

## Final decisions (locked, source of truth for implementation)

### Decision XR-D1: All `R`-type flushes — KEEP

All 14 ring/staging flushes (`[XR.R1]`–`[XR.R24]`) are cross-host
coordination and remain unchanged.  iter-21A bucket partition does not
touch the ReadRing / ReadStagingMatrix layout or the publish/spin
protocol.

### Decision XR-D2: All `B`-type flushes — REMOVE

Both bucket-flush sites in `read_handler` are owner-self accesses
(read_handler runs on owner host scanning owner's own bucket):

- **L2 NOOP branch** ([src/cxl_kv_ops_A.cc:2330-2331](../../../src/cxl_kv_ops_A.cc#L2330)):
  `[XR.B1]/[XR.B2]` — delete the `flush_line(bucket)` + `full_fence()`.
- **L0/L1 full path** ([src/cxl_kv_ops_A.cc:2349-2351](../../../src/cxl_kv_ops_A.cc#L2349)):
  `[XR.B3]/[XR.B4]/[XR.B5]` — delete the three lines.

Same rationale as LR-D1.  Partition makes the bucket owner-exclusive →
peer never writes here → same-host MOESI covers freshness.

### Decision XR-D3: All `D`-type pool accesses — split by direction (per LR-D2)

- **Owner-side** in `read_handler` ([XR.D2], `pool_->read(blk_off, hdr_buf, 4)`):
  switch to **`pool_->read_local(...)`** — receiver runs on owner host
  reading owner's own pool segment.  MOESI covers; no flush+fence.

- **Peer-side** in `forward_read_direct` ([XR.D1], `pool_->read(blk_off+4, out_buf, copy_len)`):
  switch to **`pool_->read_xhost(...)`** — peer reads owner's pool
  segment cross-host.  Requires `clflushopt + mfence` to invalidate
  peer's L1.  No cross-host CPU coherence on g1/g2 XConn switch (per
  policy P1).

### Implementation notes

1. **Same API split as LR-D2 reuses one CMake change.**  Add
   `read_local` / `read_xhost` to `cxl_kv_blockpool.{h,cc}` once,
   re-wire LR + XR call sites in the same patch.

2. **Receiver-NOOP L2 branch deletion**: when removing `[XR.B1]/[XR.B2]`,
   the L2-NOOP branch's purpose (measuring CXL line touch cost) becomes
   moot since the touch is now a same-host cache hit.  Consider whether
   to delete the L2/L3 NOOP branches altogether as part of iter-21A
   cleanup — they're benchmark-attribution scaffolding from iter-16A,
   not protocol path.  **Default recommendation**: keep them for now
   (they're env-gated by `recv_noop_level()`), retire in iter-22A.

3. **No probe (`PROBE_READ_OP("XRS…")`) is removed**; iter-18A perf
   attribution still depends on them.

4. **hazard_protect / hazard_release stay**.  HAZARD-mode peer pool
   read needs the hazard pointer regardless of cache coherence.  The
   blk_off-staying-alive guarantee comes from the bump-only pool
   freelist (iter-14A backlog item — when freelist GC lands, hazard
   protects against ABA).

---

## Open questions specifically about XR

1. **iter-20A §2 staging-protocol removal**: CONFIRMED in iter-21A scope.
   iter-20A already deleted the STAGING+RCU branches from
   `forward_read_direct` and `read_handler` hot path; what's left to
   clean up is the **XW RESERVED-exhaustion fallback to STAGING copy**
   ([src/cxl_kv_ops_A.cc:1740-1762](../../../src/cxl_kv_ops_A.cc#L1740))
   and the `rcu_` nullptr placeholder
   ([src/cxl_kv_ops_A.h:306](../../../src/cxl_kv_ops_A.h#L306)).
   Addressed in LW + XW pseudocode docs.

2. **Receiver-NOOP L2/L3 branches**: KEEP as-is.  They are bench
   attribution scaffolding (iter-16A), gated by `recv_noop_level()`
   env var, never on production path.  User-specified policy: "no
   toggle switches" refers to PRODUCTION read/write paths only — bench
   knobs survive.

3. **Bucket epoch staleness check** (`my_epoch_at_send >
   st->lookup_epoch` → -3): KEEP for now.  Partition removes the
   "peer wrote into our bucket" race but same-host worker writes can
   still bump the epoch.  Re-review in iter-22A.

---

## Important clarification — "staging" has two meanings

This naming legacy caused confusion during XR review.  Iter-21A will
disambiguate by **rename**.

| What | (A) STAGING-mode XR protocol | (B) ReadStagingMatrix data structure |
|---|---|---|
| What it is | An XR protocol mode where owner memcpy's value bytes into a shared CXL region, peer reads value from that region | A per-(req_host, owner, shard, slot) CXL memory region used as the **owner → peer control-field channel** (key, value_size, status, lookup_epoch, resp_blk_off, ready_op_id) |
| Carries value bytes? | YES (the whole point) | NO — only control fields (HAZARD mode uses pool->read_xhost for value bytes) |
| Status | DELETED by iter-20A §2 ([src/cxl_kv_ops_A.cc:2184 comment](../../../src/cxl_kv_ops_A.cc#L2184), [2366](../../../src/cxl_kv_ops_A.cc#L2366), [2434](../../../src/cxl_kv_ops_A.cc#L2434)) | STILL ALIVE — required for HAZARD mode |
| Flushes on it | Were many (value-byte cachelines) — already removed | `[XR.5/6/7/26/27/28/29]` etc. — cross-host control publish/ack |

**Rename is BACKLOG, NOT iter-21A scope** (user decision 2026-06-08).
The future rename will use a `hazard`-themed identifier — concrete
naming to be decided later.  Until that lands, the data structure
keeps its current `ReadStagingMatrix` / `ReadStagingSlot` /
`read_staging_slot` / `publish_staging` names.  Anyone reading the
code after iter-21A should know "this is the HAZARD-mode
owner→peer control channel, not the deleted STAGING-protocol value
transport".

Backlog item recorded below in §Backlog.

---

## Final decisions (locked 2026-06-08, source of truth for implementation)

### Decision XR-D1: ALL ring/response coord flushes — KEEP

29 flush+fence sites that publish/spin on cross-host ring entries +
response slot control fields (`[XR.1/2/3/4/5/6/7/8/9/10/11]` in
forward_read_direct, `[XR.13-22]` in L3/L2 NOOP branches,
`[XR.26/27/28/29]` in production publish_response).  All cross-host
coordination — partition does not affect them.

### Decision XR-D2: ALL bucket-cacheline flushes — REMOVE

- `[XR.17]/[XR.18]` (L2 NOOP `flush_line(bucket) + full_fence()`,
  [src/cxl_kv_ops_A.cc:2330-2331](../../../src/cxl_kv_ops_A.cc#L2330)):
  delete (the L2 NOOP branch itself stays — it just doesn't touch the
  bucket anymore, which makes its "measure bucket touch cost"
  attribution trivial-zero post-partition, but bench code is kept per
  user policy).
- `[XR.23]/[XR.24]/[XR.25]` (L0/L1 production `flush_line(bucket) +
  flush_line(bucket+64) + full_fence()`,
  [src/cxl_kv_ops_A.cc:2349-2351](../../../src/cxl_kv_ops_A.cc#L2349)):
  delete 3 lines outright (no `#if` wrapper).

Same rationale as LR-D1 + policy P2: partition makes the bucket
owner-exclusive → peer never writes → same-host MOESI covers.

### Decision XR-D3: pool->read direction-split (per LR-D2)

- `[XR.12]` in `forward_read_direct` (peer reads owner's pool segment):
  → `pool_->read_xhost(blk_off + 4, out_buf, copy_len)`.
- `[XR.30]` in `read_handler` (owner reads own pool for hdr_buf):
  → `pool_->read_local(blk_off, hdr_buf, 4)`.

API split lives in `cxl_kv_blockpool.{h,cc}` (LR-D2).

### Decision XR-D4: NOOP branches preserved

L2 / L3 receiver-NOOP branches stay (gated by `recv_noop_level()` env
var, never on production path).  "No toggle switches" policy applies
to production read/write paths only, not bench attribution code.

---

## Backlog (not iter-21A scope)

### B1. Rename HAZARD-mode owner→peer control channel

`ReadStagingMatrix` / `ReadStagingSlot` / `read_staging_slot` /
`publish_staging` / `read_staging_matrix_bytes` (~30 occurrences in
`cxl_kv_ops_A.cc` + `cxl_read_ring.h` neighbors).

- **Why rename**: the legacy name comes from the deleted STAGING-mode
  XR protocol where this region carried value bytes.  In HAZARD mode
  the region carries only control fields (status, vlen, lookup_epoch,
  resp_blk_off, ready_op_id) — "staging" is misleading.
- **Naming direction (TBD)**: theme around `hazard` to match the
  XR-protocol-mode name.  Concrete names deferred — pick when the
  rename lands.
- **When**: post iter-21A (a separate cleanup iter, or bundled with
  XW's `ForwardStagingMatrix` → equivalent rename for the write-path
  counterpart).
- **Risk**: pure mechanical rename, hash-diff invariant.

### B2. Receiver-NOOP L2 / L3 branch retirement

Once iter-21A bucket partition lands, the L2 NOOP branch's "measure
bucket touch" attribution becomes trivially zero (touch is same-host
cache hit).  Consider retiring L2 + L3 NOOP scaffolding entirely.
Currently env-gated, no production impact, but adds code surface.

### B3. Bucket epoch staleness check re-review

`my_epoch_at_send < st->lookup_epoch` → `-3` retry path
([src/cxl_kv_ops_A.cc:2165-2168](../../../src/cxl_kv_ops_A.cc#L2165)):
designed pre-partition to detect "owner's cache snapshot older than
peer's at send time".  Same-host worker epoch bumps still drive this
path post-partition, so the check stays meaningful.  Re-evaluate
whether iter-21A's reduced cross-host write coupling changes the
likelihood / value of this path.