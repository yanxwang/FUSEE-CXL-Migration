# iter-20A plan

**Status**: drafting (2026-06-02)
**Drives from**: iter-19A close-out + Q1/Q2/Q4 discussion 2026-06-02

iter-20A scope this doc covers:
1. **§1 Per-host bucket partition** — architectural change that removes the
   need for owner-self-write CXL flush+fence (G3 retire_slot, G4+G5
   publish_slot_cow, G6 pool->write, XR.R3 receiver bucket flush).
2. **§2 Code cleanup** — remove STAGING + RCU branches from xhost_read,
   STAGING + BATCHED branches from xhost_write, TLS cache from read/write
   paths. Leave HAZARD as the only xhost_read default, RESERVED as the
   only xhost_write default. Validation gated by HAZARD/RESERVED
   functional equivalence on g1/g2 (or fix HAZARD/RESERVED until
   equivalent — see §2 STAGE 1 gate).
3. **§3 Anomaly A fix** (carried from iter-19A) — change FUSEE_CACHE_BUCKETS
   default from 2097152 (c100) to 16384 (c1). Data-validated 3.3× cluster
   gain on workload-A T=64 zipf-0.99.
4. **§4 G2 ship** (carried from iter-19A Phase 3) — default
   `FUSEE_LR_DEL_POOL_READ_FLUSH=1` (LR pool->read flush removal).
   Modest +3.0% c100 thpt, hash-diff PASS.

This doc lays out §1 in detail; §2-§4 are summarized + reference their
iter-19A source docs.

---

## §1 Per-host bucket partition

### 1.1 Problem statement

Protocol A's hashtable is on CXL as a single shared array `buckets_[num_buckets]`.
Bucket lookup uses `bucket_idx(key) = fnv1a_u64(key) % num_buckets`, which
is **independent of `owner_host(key)`**. As a consequence, a single bucket
can contain slots whose keys belong to **different owner hosts**, and a
slot's owner can change (DELETE → reassign-to-new-key) over time.

This creates a cross-host visibility requirement on **every owner-self write**
to a slot: peer hosts must be able to observe the new slot content (or the
DELETE clearing the slot) via direct CXL reads, because peer hosts may
themselves scan the same bucket to insert their own owned keys.

The iter-19A Phase 3 flush+fence audit
([iter19A_flush_fence_audit_v2.md](iter19A_flush_fence_audit_v2.md))
catalogued the required cross-host flushes on the LW path:

| Group | Site | Why it's required today |
|---|---|---|
| **G3** | `retire_slot()` flush+sfence | Peer h that wants to INSERT a key K' with `bucket_idx(K') == bucket_idx(deleted_K)` and `owner_host(K')==h` must observe the slot as empty (slot.key == kEmptyKey). Without flush, peer reads stale CXL data and may miss the empty slot. |
| **G4+G5** | `publish_slot_cow()` flush+sfence (inline + blockpool) | Peer h that wants to read key K (forward_read) — owner's `read_handler` runs on owner host (MOESI safe), BUT also peer h that wants to INSERT a key K' with the same bucket index must observe the slot's CURRENT key — to avoid trying to reuse an occupied slot or duplicate the key. |
| **G6** | `pool->write()` per-cacheline flush+sfence | Peer h that does forward_read on a key K written by owner — receiver runs on owner host so MOESI covers, BUT the peer's eventual `pool->read` from HAZARD-mode resp_blk_off pulls block bytes from owner's CXL segment directly. (Caveat: in current STAGING default this isn't actually triggered; HAZARD/RESERVED swap changes this.) |
| **XR.R3** | `read_handler` bucket flush+mfence | Same as LR.1 / B-H3 — receiver on owner host reading owner's own bucket; MOESI safe; **already a candidate for removal post-Q1**. |

The iter-19A Phase 3 empirical test confirmed G45 cannot be removed
under the current bucket layout: `bnf-G45` build HANGS at 2-host T=32
with 8M-bucket hash-diff workload (publish_slot_cow flush removal
breaks the protocol). G6 yields no measurable thpt gain on single-host
(`-1.5% c100`) and carries unverified cross-host risk. G3 untestable
without DELETE workload.

### 1.2 Proposed redesign

**Partition the bucket array across hosts** so that every bucket has
exactly one owner. Choose either of two equivalent encodings:

**Option A — co-located `bucket_idx` formula** (simplest, no layout change):

```cpp
constexpr uint32_t kNumBucketsPerHost = num_buckets / num_hosts;

uint32_t bucket_idx(uint64_t key) {
  uint32_t owner = owner_host(key);
  uint32_t local = fnv1a_u64(key) % kNumBucketsPerHost;
  return owner * kNumBucketsPerHost + local;
}
```

Same `buckets_[num_buckets]` CXL array; bucket index just happens to
encode owner in the high bits. No physical layout change.

**Option B — physically separate CXL segments per host** (matches the
blockpool layout convention):

```
CXL bucket region layout:
  segment[0] = buckets_for_host_0[kNumBucketsPerHost]
  segment[1] = buckets_for_host_1[kNumBucketsPerHost]
  ...
  segment[H-1] = buckets_for_host_{H-1}[kNumBucketsPerHost]
```

Each host's segment is logically "owned" by that host. Only the owner
host writes to its segment; peer hosts only read (and only via the
`read_handler` running on the owner host for cross-host reads).

Both options achieve the same invariant: **for any key K, the bucket
holding K is always written by `owner_host(K)` exclusively**.

Recommended: **Option A** (formula-only) for iter-20A. Option B is
cleaner architecturally but requires changes to attach / init / hash-diff
dump paths. Option A drops in with one function-body change.

### 1.3 Expected benefits — flush+fence elimination

After per-host bucket partition, all owner-self writes to a slot are
single-writer + same-host coherent for any reader on the owner host.
Cross-host reads of those slots still happen via `read_handler` on
the owner host, which is also same-host coherent. Therefore:

| Site | Current verdict (iter-19A) | Post-partition verdict |
|---|---|---|
| **LW.1** retire_slot flush+sfence (G3) | ⚠ peer-empty-slot visibility | ❌ **Removable** — no peer scans this bucket |
| **LW.2/3** publish_slot_cow flush+sfence (G4+G5) | ❌ HANGS at 2-host | ❌ **Removable** — owner-self MOESI suffices |
| **LW.4** pool->write per-cacheline flush+sfence (G6) | ⚠ peer-block visibility under HAZARD | ❌ **Removable** — block pool is per-host, peer reads through owner's `read_handler` (HAZARD: peer fetches blk_off ptr, but pool block was written by owner so MOESI covers when receiver scans it for HAZARD; further details depend on HAZARD impl, see §1.5) |
| **XR.R3** xhost_read receiver bucket flush+mfence | iter-19A candidate, untested | ❌ **Removable** — receiver always on owner host |
| **LR.1** owner-self bucket pre-scan (B-H3) | ✅ Already removed | (already removed; consistency check) |
| **LR.2** pool->read per-cacheline flush+mfence (G2) | iter-20A ship candidate | ✅ Already on ship list |

Net effect: **the entire LW path becomes flush-free on the worker side**
(LWS1-LWS6 only touch DRAM directory + DRAM cache_pool); the cross-host
flushes remaining are only the ring/staging control-plane operations
(XWS1, XWS4, XRS1, XRS3, XRS4, XRS5, XRS6, XRR ring drain, XWR ring drain)
which are inherent to cross-host RTT.

### 1.4 Tradeoffs and risks

**PRO**:
- Removes all owner-self write CXL flush+sfence — potentially large
  thpt gain on write-heavy workloads (workload-A 50% writes, workload-D
  with insertions). Phase 3 G45+G6 estimated ceiling: 4-8% thpt on
  workload-A but likely larger under specific access patterns
  (UPDATE-heavy single-host single-thread microbench has very different
  shape — TBD by sweep).
- Eliminates G45 HANG risk — flush isn't required, so removal is safe.
- Simplifies cross-host correctness reasoning: owner = single writer
  for both data (slot.value/key) AND container (bucket).

**CON / risk**:
- **Hashtable load imbalance under skewed owner_host distribution**.
  Current design hashes uniformly across all buckets regardless of
  owner_host distribution; per-host partition concentrates owner_host(K)'s
  ops onto its own bucket region. If owner_host distribution is uniform
  (current sharding hash is uniform), each host's bucket region has the
  same expected load → no per-host hot region.
  Under zipf-skewed key distributions, the hot keys are still spread
  across the same bucket count per host as before, just within owner's
  region. **No additional load imbalance vs current design**.
- **Resize / rebalance complexity**. Future ops that change H (add/remove
  host) would require migrating buckets across segments. Defer to
  iter-21A+ if needed; iter-20A assumes H fixed.
- **Migration cost**. Existing on-CXL hashtable contents need to be
  re-distributed to match the new bucket_idx formula. For an empty
  init (test fresh starts) — trivial. For migrating live data — a
  one-time `re-hash + move` pass. Defer until users hit a live-data
  use case.
- **iter-19A audit doc updates**. The audit's LW.1-LW.4 verdicts
  ("⚠ peer visibility required") become ❌ removable post-partition.
  Doc needs a "post-iter-20A" amendment column.

### 1.5 What still needs flush+sfence post-partition

Post-partition, the only remaining flush+sfence sites are on the
cross-host RTT control plane:

| Stage | Site | Why still required |
|---|---|---|
| XWS1, XRS1 | ring tail flush+sfence on slot_reserve | Receiver on peer host needs to see updated tail |
| XWS2, XRS2 | per-slot req_op_id flush+mfence on slot_wait spin | Receiver/next-sender across hosts |
| XWS3 ForwardStaging value flush+sfence | (only if STAGING write mode retained; in RESERVED mode it's the peer's pool->write to its own peer-sub-region of owner's segment which DOES need flush+sfence — peer-write to CXL block that owner's receiver will read) | Cross-host visibility |
| XWS4 req_op_id publish flush+sfence | Receiver picks up | Cross-host visibility |
| XRS3 req publish + staging clear flush+sfence | Receiver pickup + reader-side spin order | Cross-host visibility |
| XRS4 ready_op_id flush+mfence (spin) | Receiver wrote ready_op_id, sender reads | Cross-host visibility |
| XRS5 req_op_id=0 flush+sfence (release slot) | Next sender on same slot | Cross-host visibility |
| XRS6 staging value bytes flush+mfence | In STAGING mode receiver wrote value bytes; sender reads them | Cross-host visibility (only STAGING; HAZARD/RESERVED use pool->read with its own flush from §1.4 reduction) |
| XRR1, XWR6 ring drain flush+mfence | Receiver reads sender's ring writes | Cross-host visibility |
| XRR2 bucket flush, XR.R3 | (NOW removable post-partition — receiver runs on owner host) | ❌ removed |
| XRR3, XWR8 resp/ready flush+sfence | Sender reads receiver's ack | Cross-host visibility |

The ring control plane is inherent to cross-host RTT; cannot be reduced
without protocol changes. iter-20A target is removing the owner-self
data-plane flushes only.

### 1.6 Implementation sketch (Option A, formula-only)

**Phase 1 — bucket_idx formula** (single function):
- Edit `bucket_idx()` in `src/cxl_kv_ops_A.cc` (or wherever it lives —
  TBD, may be inline in header)
- Add compile flag `FUSEE_BUCKET_PARTITION` defaulting to 0 (preserve
  current behavior); flip to 1 to enable
- `num_buckets` constraint: must be divisible by `num_hosts`. Add
  static_assert or runtime check in attach.

**Phase 2 — owner-self invariant assert (debug build)**:
- In `execute_write_local`, assert `bucket_idx(key) / kNumBucketsPerHost == host_id_`
- In `read_handler` (xhost_read receiver), assert `bucket_idx(key) / kNumBucketsPerHost == host_id_`
- Catches bugs where `owner_host` and `bucket_idx` formulas drift.

**Phase 3 — sharer_bitmap simplification**:
- Since peer hosts never directly read owner's slot/block, `sharer_bitmap`
  semantics simplify: it's now just a "which hosts have cached this key"
  for InvalRing purposes, set by `read_handler` when peer registers.
- No code change needed in iter-20A (semantics same), but document.

**Phase 4 — flush+fence removal**:
- Drop `retire_slot()` flush+sfence (LW.1) — gated by build flag
  `FUSEE_BUCKET_PARTITION` since the removal is only safe under partition
- Drop `publish_slot_cow()` flush+sfence (LW.2/3)
- Drop `pool->write()` flush+sfence (LW.4)
- Drop `read_handler` bucket flush (XR.R3)

**Phase 5 — hash-diff battery**:
- Standard 5-rep T={8,32,64} workload-a + workload-c + workload-d
- All must PASS byte-identical
- Plus: post-partition workload-d (DELETE-heavy) **must not regress**
  — DELETE was the case where G3 flush was hypothesized to matter.

**Phase 6 — thpt sweep**:
- workload-a + workload-c at T=64, V=1024, zipf-0.99, c1+c100, 3 reps
- Compare to iter-19A bnoflush baseline
- Expected: ≥ baseline (no regression); ideal: measurable gain from
  removed flushes

### 1.7 Verification plan (gates)

| Gate | Criterion | Block iter-20A close on fail |
|---|---|---|
| G1 | Compile + smoke (T=8, 100k ops, 2-host) | ✅ |
| G2 | Cross-host hash-diff workload-a/c/d × T={8,32,64} × 5 reps PASS | ✅ |
| G3 | workload-A T=64 zipf-0.99 cluster thpt ≥ iter-19A bnoflush baseline | ✅ |
| G4 | workload-C T=64 zipf-0.99 cluster thpt ≥ iter-19A bnoflush baseline | ✅ |
| G5 | workload-D thpt and correctness (DELETE-heavy) within 5% of workload-A baseline | ✅ |
| G6 | LWS probe decomp confirms LWS4 latency ≈ 0 (pool->write + publish_slot_cow no longer flushing) | reportable, not blocking |

### 1.8 Estimated scope

- Phase 1-4 source changes: ~50 LOC (formula + asserts + flush removals)
- Phase 5 hash-diff battery: existing scripts, 5-10 min to run
- Phase 6 sweep: 5 builds × 2 workloads × 2 cells × 3 reps = 60 cells ~ 1 hour
- Total: 1-2 hours of testbed once code lands.

### 1.9 References

- iter-19A Phase 3 flush+fence isolation: confirms G45 HANG is the bound
  the current design hits; per-partition removes the bound.
  [iter19A_phase3_flush_iso_summary.md](iter19A_phase3_flush_iso_summary.md)
- iter-19A audit by 4 paths: per-site cross-host visibility analysis.
  [iter19A_flush_fence_audit_v2.md](iter19A_flush_fence_audit_v2.md)
- 4-path stage decomp (consumed for stage labels): same iter-19A doc set.
  [iter19A_4path_stage_decomposition.md](iter19A_4path_stage_decomposition.md)

---

## §2 Code cleanup — STAGING/RCU/BATCHED/TLS removal

**Source of truth for status**: this iter (2026-06-02 onward).

### 2.1 Current state (default build, FUSEE_READ_GUARD=0, FUSEE_WRITE_ALLOC=0)

- xhost_read: STAGING mode — `read_handler` writes value bytes into
  `ReadStagingMatrix[req][owner][shard][slot].value_bytes`; sender does
  16-cacheline flush + memcpy from staging
- xhost_write: STAGING mode — sender writes value bytes into
  `ForwardStagingMatrix[me][dst][shard][slot].bytes`; receiver memcpy
  from staging → pool block → publish slot
- TLS: `FUSEE_DISABLE_TLS=1` default (TLS code present but skipped at
  runtime via `#if !FUSEE_DISABLE_TLS` blocks)

### 2.2 Goal state (post-iter-20A)

- xhost_read: HAZARD mode only — `read_handler` writes `resp_blk_off`
  into staging control fields; sender does `pool->read(blk_off + 4, ...)`
  directly from owner's segment. Hazard pointer protects against
  ABA / use-after-free.
- xhost_write: RESERVED mode only — sender pre-allocates blk_off via
  `pool->alloc_peer(owner)` (DRAM-local bump on peer's sub-region of
  owner segment), writes value bytes to the reserved block, sends only
  `(key, blk_off, op_kind, vlen)` to owner via WriteRing.
- TLS: deleted from source (file `cxl_tls_cache.h` removed; all `#if !FUSEE_DISABLE_TLS`
  blocks removed; `g_thread_tls` reference removed; `FUSEE_DISABLE_TLS`
  flag removed).
- ReservationRing: deleted (only BATCHED used it).
- `cxl_read_guard.h`: STAGING + RCU mode-switch code deleted; HAZARD
  becomes unconditional.

### 2.3 Execution order (3 stages)

**STAGE 1 — verify HAZARD + RESERVED (no source deletion)**

- Build `build-cxl-w1-v1024-hzres` on g1+g2 with
  `-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1`
- Cross-host hash-diff (8M buckets, T=32, 500k workload-a ops): must
  PASS byte-identical
- Thpt sweep workload-a + workload-c at T=64, V=1024, c1+c100, 3 reps
  → compare to current STAGING default

Gate decision (user-directed): **if hash-diff FAIL or thpt regress, do
NOT abandon the plan — instead debug + fix HAZARD/RESERVED until
equivalent** (latent bugs in HAZARD/RESERVED that have never surfaced
because they were never default).

**STAGE 2 — source deletion (after STAGE 1 gate passes)**

Touch files: `src/cxl_kv_ops_A.{h,cc}`, `src/cxl_read_guard.h`,
`src/cxl_kv_ops_A.cc` (search()/forward_read_direct/read_handler/
forward_write_direct/write_handler/execute_write_local),
`src/cxl_tls_cache.h` (delete file). Also possibly
`src/cxl_forward_staging.h`, `src/cxl_read_staging.h`,
`src/cxl_reservation_ring.h`, `tests/protocol_a_ycsb.cc` (TLS init
calls), `src/cxl_kv_ops_A.h` (enable_*_ring signatures).

Estimated LOC delta: -500 to -1500.

**STAGE 3 — re-verify post-deletion**

Same hash-diff + thpt sweep as STAGE 1. Result must match STAGE 1
(±noise). Any deviation → roll back the offending edit.

### 2.4 Risk

- HAZARD/RESERVED never battle-tested at scale. iter-13A introduced
  them but never made them default. Possible latent bugs:
  - HAZARD: ABA protection might have edge cases not covered by
    iter-13A test suite (the suite was 4-path microbench, not YCSB
    workload).
  - RESERVED: `pool->alloc_peer` per-peer sub-region size — current
    `peer_blocks_per_peer_` value might be too small for 2M trans ops,
    causing exhaustion. Need to check + tune.

### 2.5 Time budget

- STAGE 1 build + hash-diff + sweep: ~45 min
- STAGE 1 debug (if failures): variable, budget 1-3 hours
- STAGE 2 source deletion: ~1-2 hours of careful editing
- STAGE 3 verify: ~45 min
- Total: 3-5 hours if HAZARD+RESERVED work out of the box, longer if bugs

---

## §3 Anomaly A fix (cache_buckets default)

Carried from iter-19A. Change `FUSEE_CACHE_BUCKETS` default
2097152 → 16384 (= cache_pct 1%, "mixed regime"). Data-validated 3.3×
cluster gain at T=64 workload-a zipf-0.99.

References:
- [iter19A_anomaly_a_consolidated.md](iter19A_anomaly_a_consolidated.md) §4 Fix path
- [iter19A_phase3_flush_iso_summary.md](iter19A_phase3_flush_iso_summary.md) §7

Implementation: ~3 LOC (constant change + a call-site default in
`tests/protocol_a_ycsb.cc`). Bundle with §2 cleanup PR.

---

## §4 G2 ship (LR pool->read flush removal)

Carried from iter-19A Phase 3. Default `FUSEE_LR_DEL_POOL_READ_FLUSH=1`.

References: [iter19A_phase3_flush_iso_summary.md](iter19A_phase3_flush_iso_summary.md)

Implementation: 1 LOC default change in `src/cxl_kv_blockpool.cc`.
Bundle with §2 cleanup PR.

---

## Open question parking lot

- Phase 6 / §1.7 gate G6 — LWS probe analyzer doesn't yet exist
  separately from LRS analyzer. Defer probe-level verification to
  iter-20B if time-budget tight.
- workload-d traces — need to confirm `setup/workloads/workloadd.spec_*`
  exists on g1/g2. If not, generate from YCSB.
