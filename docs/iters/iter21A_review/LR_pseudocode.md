# LR — Local Read pseudocode (review surface for iter-21A flush removal)

**Path**: `search(key, out_buf, buf_len, out_len)` when `owner_host(key) == self`.

**Source**: [src/cxl_kv_ops_A.cc:2602-2748](../../../src/cxl_kv_ops_A.cc#L2602) `CxlKvStoreA::search` + [src/cxl_kv_blockpool.cc:192-209](../../../src/cxl_kv_blockpool.cc#L192) `CxlKvBlockPool::read` + [src/cxl_cache_pool.cc::cache_pool_lookup](../../../src/cxl_cache_pool.cc).

**Annotation key**:
- `[…]` tag = local label so we can refer to a specific flush during review
- `pre-partition`: the iter-19A end-state reason
- `post-partition`: my recommendation for iter-21A (REMOVE / KEEP / NEEDS DISCUSSION)

---

## LR pseudocode

```
search(key, out_buf, buf_len, out_len):
  if key == 0: return -1

  # ─── L2: shared DRAM cache_pool lookup (FUSEE_DISABLE_CACHE_POOL=0 by default) ───
  PROBE("LRS2S")
  if cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz):    # [LR.C1] per-bucket seqlock CAS, no CXL flush
      PROBE("LRS2H")                                           #   NOTE: cache_pool is DRAM-resident.
      *out_len = sz; memcpy(out_buf, buf, sz)                  #   Seqlock = no mutex on reader.
      return 0                                                 #   pre-partition: no flush here, nothing to change
  PROBE("LRS2M")                                               #   post-partition: N/A

  # ─── L3: bucket-scan path ───
  owner = owner_host(key)                                       # high-bit FNV-1a slice

  if owner != self:                                             # XR path
      return forward_read(owner, key, ...)                      # → §XR pseudocode

  # ─── owner-self miss: direct CXL bucket scan ───
  b  = bucket_idx(key)                                          # pre-partition: hash % num_buckets
                                                                # post-partition: owner*N_per_host + (hash % N_per_host)
  bucket = &buckets_[b]

  # [LR.B1] cxl_kv_ops_A.cc:2707  flush_line(bucket)
  # [LR.B2] cxl_kv_ops_A.cc:2708  flush_line(bucket + 64)
  # [LR.B3] cxl_kv_ops_A.cc:2711  full_fence()
  #   pre-partition reason: peer might have INSERTed into a DIFFERENT slot of
  #     this same bucket (its key maps to same bucket_idx, possibly diff owner).
  #     Without flush, owner's L1 might serve a stale cacheline that misses
  #     the peer's slot.
  #   post-partition decision: ★ REMOVE all three ★
  #     After partition, no peer ever writes to buckets in owner's segment.
  #     Owner's local CPUs maintain MOESI within the host → load is fresh.
  #     iter-19A Phase 2 already added env knobs (FUSEE_LR_DEL_OWNER_FLUSH /
  #     FLUSH_ONLY / FENCE_ONLY) to ablate this; iter-21A turns the removal
  #     into the unconditional default + drops the #if/#endif.

  for s in 0..6:                                                # kCxlKvSlotsPerBucket=7
      slot = bucket->slots[s]
      if slot.key == key:
          encoded = slot.value                                  # 8 B: size_class | blk_off | inline_value
          sc = slot_size_class(encoded)
          if sc == kSizeClassInline or pool_ == nullptr:        # inline tiny class
              vbuf = encoded; vlen = 8
          else:                                                 # variable-length block fetched from pool
              blk_off = slot_blk_off(encoded)
              pool_->read(blk_off, hdr_buf, 4)                  # ←──── pool->read trampolines through
              vlen = parse_u32(hdr_buf)                         #         [LR.P1] cxl_kv_blockpool.cc:203 flush_line(p)
              pool_->read(blk_off + 4, vbuf, vlen)              #         [LR.P2] cxl_kv_blockpool.cc:206 full_fence()
                                                                #   pre-partition reason: peer's forward_write running on
                                                                #     owner host's write_handler→pool->write(blk) writes
                                                                #     bytes to owner's pool segment. Owner's later own
                                                                #     pool->read needs to see those bytes.
                                                                #     BUT: write_handler runs on owner host → MOESI safe
                                                                #     within the host. The flush is defensive for the
                                                                #     case where ANOTHER host's pool->read reads owner's
                                                                #     block directly (e.g., HAZARD mode).
                                                                #   post-partition: pool ownership is unchanged by bucket
                                                                #     partition. Pool segments are already per-host. So
                                                                #     [LR.P1] / [LR.P2] are NOT a partition consequence —
                                                                #     they're about HAZARD/RESERVED read modes.
                                                                #     ★ Already env-gated by FUSEE_LR_DEL_POOL_READ_FLUSH.
                                                                #       iter-20A §4 plan was to ship the default flip
                                                                #       (1=skip flush) — confirm it shipped, else flip
                                                                #       as part of iter-21A.
          *out_len = vlen
          memcpy(out_buf, vbuf, min(vlen, buf_len))
          cache_pool_insert(cache_, key, vbuf, vlen)            # repopulate DRAM cache (no CXL flush)
          return 0
  return -1                                                     # not found
```

---

## Summary — LR partition impact

| tag | site | flush | pre-partition reason | post-partition decision | effort |
|---|---|---|---|---|---|
| `[LR.C1]` | cache_pool seqlock | n/a (DRAM) | — | — | n/a |
| `[LR.B1]` | `cxl_kv_ops_A.cc:2707` | `flush_line(bucket)` | peer's INSERT into same bucket from owner's view | **REMOVE** | drop `#if !FUSEE_LR_DEL_FLUSH_ONLY` block |
| `[LR.B2]` | `cxl_kv_ops_A.cc:2708` | `flush_line(bucket + 64)` | same | **REMOVE** | same |
| `[LR.B3]` | `cxl_kv_ops_A.cc:2711` | `full_fence()` | bracket B1+B2 | **REMOVE** | drop `#if !FUSEE_LR_DEL_FENCE_ONLY` block |
| `[LR.P1]` | `cxl_kv_blockpool.cc:203` | `flush_line(p)` per cacheline | HAZARD-mode peer pool-read | **NEEDS DISCUSSION** (already env-gated, iter-20A §4 wanted ship-1) |
| `[LR.P2]` | `cxl_kv_blockpool.cc:206` | `full_fence()` | bracket P1 | **NEEDS DISCUSSION** (same) |

**iter-21A action items from LR**:
1. Drop `[LR.B1]/[LR.B2]/[LR.B3]` unconditionally (kill the env knobs too — partition makes them unambiguous).
2. Confirm `[LR.P1]/[LR.P2]` are removed via the iter-20A §4 ship (or do it here).

---

## Open questions specifically about LR

1. **Cache pool seqlock retry on partition?** When we remove the bucket
   flush, the DRAM cache_pool path stays unchanged.  But after a peer-host
   INSERT that propagates into owner's cache via the
   `cache_pool_insert_from_inval` path (or wherever the invalidate
   handler updates the cache), does the seqlock interaction still work?
   This is orthogonal to bucket partition but worth a once-over.
2. **`cache_pool_lookup` value copy**: not on the flush path, but the
   `out_buf >= kForwardStagingSlotBytes` (1088 B) gating in
   `FUSEE_LR_DEL_DOUBLE_COPY` saves a 1088 B memcpy on every HIT.  Ensure
   the iter-21A benchmark binary uses a 1088 B rbuf so this fast path is
   taken.  (Confirmed in iter-19A Phase 1d H14.)
3. **No flush before the slot.key read inside the for-loop**: We're
   removing [LR.B1-3], which means the loop reads `bucket->slots[s].key`
   from cache.  Same-host MOESI ensures any write by an owner-host
   thread (this thread or another worker on the same host) is visible
   without flush.  No peer ever writes to this bucket.  → safe.

---

## Final decisions (locked 2026-06-08, source of truth for implementation)

### Decision LR-D1: `[LR.B1] / [LR.B2] / [LR.B3]` — REMOVE unconditionally

- **Sites**: [src/cxl_kv_ops_A.cc:2707-2711](../../../src/cxl_kv_ops_A.cc#L2707).
- **Edit**: Delete the entire `#if !FUSEE_LR_DEL_OWNER_FLUSH ... #endif`
  block (lines 2705-2713 in current code; line numbers may shift).
  Also delete the env-knob CMake plumbing
  ([src/CMakeLists.txt:110-114](../../../src/CMakeLists.txt#L110)) for
  `FUSEE_LR_DEL_OWNER_FLUSH`, `FUSEE_LR_DEL_FLUSH_ONLY`,
  `FUSEE_LR_DEL_FENCE_ONLY` — partition makes them unambiguous, no
  more A/B ablation needed.
- **Why safe**: post-partition, this bucket is in owner's segment;
  peer never writes to it; same-host MOESI keeps owner-side L1
  coherent.

### Decision LR-D2: `[LR.P1] / [LR.P2]` — split pool->read API by direction

**Important correction**: CXL 2.0 Type-3 device behind the XConn switch
on g1/g2 does **NOT** provide cross-host CPU cache coherence.  When
owner does `clflushopt` on a pool block, it pushes the write to CXL
memory but does NOT invalidate any peer host's L1 line that happened
to be loaded earlier.  Peer must do its OWN `clflushopt + mfence`
before loading to fetch the fresh bytes from CXL.

iter-19A Phase 3 `FUSEE_LR_DEL_POOL_READ_FLUSH=1` empirical validation
(+3.0 % c100 thpt, hash-diff PASS) only covered the **owner-self** read
path — peer-side HAZARD-mode read of remote pool segment was never on
that test surface.

**Edit**: Split `CxlKvBlockPool::read` into two methods at
[src/cxl_kv_blockpool.h](../../../src/cxl_kv_blockpool.h) +
[src/cxl_kv_blockpool.cc](../../../src/cxl_kv_blockpool.cc):

```cpp
// Local read — caller is reading its own host's pool segment.
// MOESI on same host guarantees freshness; no clflushopt + mfence.
void CxlKvBlockPool::read_local(uint64_t off, void *out, uint32_t len) const {
  if (off == 0 || len == 0 || len > block_size_) return;
  std::memcpy(out, base_ + off, len);
}

// Cross-host read — caller is reading another host's pool segment.
// Local L1 may have a stale cacheline; must clflushopt + mfence so
// the subsequent memcpy fetches from CXL memory.
void CxlKvBlockPool::read_xhost(uint64_t off, void *out, uint32_t len) const {
  if (off == 0 || len == 0 || len > block_size_) return;
  const uint8_t *src = base_ + off;
  uint8_t *p = (uint8_t *)((uintptr_t)src & ~(uintptr_t)63);
  uint8_t *end = (uint8_t *)(src + len);
  while (p < end) { flush_line(p); p += 64; }
  full_fence();
  std::memcpy(out, src, len);
}
```

Then update call sites:
- [src/cxl_kv_ops_A.cc:2727, 2730](../../../src/cxl_kv_ops_A.cc#L2727)
  in `search()` (LR owner-self) → `pool_->read_local(...)`.
- [src/cxl_kv_ops_A.cc:~2300+](../../../src/cxl_kv_ops_A.cc#L2300)
  in `read_handler` (XR receiver runs on owner host, reads owner's own
  pool) → `pool_->read_local(...)`.
- Future HAZARD-mode peer pool read (in iter-21A staging-removal scope
  — see XR pseudocode) → `pool_->read_xhost(...)`.

Delete the `FUSEE_LR_DEL_POOL_READ_FLUSH` env knob
([src/CMakeLists.txt:117-118](../../../src/CMakeLists.txt#L117)) — the
owner/peer distinction is now baked into the call site.

The original `read()` method can be (a) kept as an alias for
`read_local` during transition, or (b) deleted outright with all
callers updated.  Recommendation: (b) — delete to force every caller
to declare intent.

### Decision LR-D3: `[LR.C1]` cache_pool seqlock — no change

- DRAM-host-local, per-entry seqlock CAS, orthogonal to CXL coherence
  and bucket partition.  Leave the cache_pool subsystem alone in
  iter-21A.

### Broader policy implications

These principles surfaced during LR review and apply across all 4
paths:

**P1. CXL Type-3 (g1/g2) has no cross-host CPU cache coherence.**
Every cross-host read MUST do `clflushopt + mfence` before load.
Every cross-host write MUST do `clflushopt + sfence` after store.
Owner-self accesses inside one host's L1 domain rely on standard x86
MOESI — no explicit flush/fence needed there.

**P2. Per-host bucket partition (§1.4 of iter-21A constraints) turns
every bucket access into an owner-self access by construction.**  All
flush/fence on `buckets_[…]` cachelines become removable.

**P3. Pool block segments are already per-host** (allocator bumps
per-host cursors, blocks live in owner host's pool slice).  Owner's
own pool reads are owner-self (P1).  Peer's pool reads in HAZARD-mode
XR are cross-host (P1).  → split the API by direction.

**P4. Ring / staging / inval entries are by definition cross-host**
(producer and consumer on different hosts).  Their flush/fence stays
in place under any iter-21A change.

**P5. `pool->write` stays as-is (keep flush+sfence) regardless of who
writes.**  Owner writes to its own segment; peer's subsequent
cross-host `read_xhost` requires the bytes to be durable in CXL
before owner publishes the ring ACK with the block_off.  Cleanup:
delete the `FUSEE_LW_DEL_POOL_WRITE_FLUSH` env knob (always-on, no
A/B).

### Implementation notes (for the patch authoring turn)

- All three env knobs (`FUSEE_LR_DEL_OWNER_FLUSH`, `FLUSH_ONLY`,
  `FENCE_ONLY`) + `FUSEE_LR_DEL_POOL_READ_FLUSH` +
  `FUSEE_LW_DEL_POOL_WRITE_FLUSH` go away in one CMake cleanup commit
  AFTER the source-side `#if` removal is verified by hash-diff.
- The split `read_local` / `read_xhost` methods are added before any
  call-site rewires, so hash-diff + YCSB-A c=16 sanity passes at every
  bisection step.
- No probe (`PROBE_LR_OP("LRS3S/E")`) is removed; iter-19A perf
  attribution still needs them.
