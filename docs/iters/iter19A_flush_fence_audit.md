# iter-19A — local_read + local_write flush+fence audit

**Purpose**: enumerate every CXL `flush_line` / `clflushopt` and every
`store_fence` / `full_fence` (`sfence` / `mfence`) on the
`execute_write_local` (LWS) and `search` owner-self (LRS) hot paths, then
analyze theoretically which are necessary under the local coherence
domain assumption.

**Scope of "in path"**:
- local_write = `CxlKvStoreA::execute_write_local()` and every callee
  it invokes on the success path (no early-return error).
- local_read = `CxlKvStoreA::search()` owner-self HIT (LRS2H return)
  and owner-self MISS (LRS3/LRS4 return) sub-paths. Cross-host MISS
  (forward_read) is OUT of scope — that path is local_read+xhost_read
  hybrid and is decomposed under XRS.

**Definitions**:
- **CXL-resident** memory: memory backed by `/dev/dax0.0` and visible
  cross-host. Cross-host cache coherence is NOT provided by hardware;
  visibility requires sender to `clflushopt`+`sfence` and reader to
  `clflushopt`+`mfence`+load (see [`docs/design_goals.md`] §I12 +
  `cxl_kv_blockpool.h:29-31`).
- **Local coherence domain**: x86 MOESI within a single host. Same-host
  workers (forked from the same parent), receiver threads, and sender
  threads observe each other's writes via L1/L2/L3/DRAM coherent caches
  with no software flushing needed. MAP_SHARED|MAP_ANONYMOUS DRAM is in
  this domain.
- **Owner-host invariant** (Protocol A spec §I9): the owner host is the
  only writer for a given key. Same-host workers contend via the slot
  directory pthread_spinlock. Peer hosts read via forward_read +
  cache_pool register; they never directly mutate the owner's bucket.

---

## Part 1 — Enumeration

### Local READ — owner-self HIT path (LRS2 returns)

| # | Site (src:line) | Code | Intent (original) | Operand domain |
|---|---|---|---|---|
| — | `cxl_cache_pool.cc` (cache_pool_lookup) | none | DRAM-resident seqlock + memcpy | DRAM |
| — | `cxl_tls_cache.h` (tls_lookup, tls_insert) | none | per-thread DRAM | DRAM |

**HIT path has ZERO flush+fence.** All operands are DRAM (cache_pool
and TLS) and live entirely in the local coherence domain. Confirms
that LRS2H sub-path is flush-free by construction.

---

### Local READ — owner-self MISS path (LRS3 + LRS4)

| # | Site (src:line) | Code | Intent (original) | Operand domain |
|---|---|---|---|---|
| **LR-F1** | `cxl_kv_ops_A.cc:3032` | `flush_line(bucket)` | invalidate L1 copy of bucket cacheline 1, force re-fetch from CXL so peer-host writes are seen | CXL bucket |
| **LR-F2** | `cxl_kv_ops_A.cc:3033` | `flush_line((char *)bucket + 64)` | same, for cacheline 2 of the 128B bucket | CXL bucket |
| **LR-N1** | `cxl_kv_ops_A.cc:3036` | `full_fence()` (mfence) | order the 2 clflushopt before the subsequent load (bucket scan) | barrier |
| **LR-F3..Fn** | `cxl_kv_blockpool.cc:189` (in `pool->read`) | per-cacheline `flush_line(p)` over the value bytes (1024 B → 16 cachelines) | invalidate L1 copies of the block bytes, force re-fetch from CXL | CXL block |
| **LR-N2** | `cxl_kv_blockpool.cc:192` (in `pool->read`) | `full_fence()` (mfence) | order the per-cacheline clflushopts before the value `memcpy` | barrier |

**LR-F1/F2/N1 are GATED** by `FUSEE_LR_DEL_OWNER_FLUSH` build flag
(iter-19A B-H3 fix). bnoflush build = OFF (removed). LR-F3..N2 are
inside `pool->read` and currently unconditional.

---

### Local WRITE — `execute_write_local()` success path

#### LWS1 entry+slotscan (line 532-568)

| # | Site (src:line) | Code | Intent (original) | Operand domain |
|---|---|---|---|---|
| — | (none) | — | iter-17A removed pre-scan bucket flush+mfence (was 2×clflushopt + mfence at the iter-16A site). Comment at line 545-547. | — |

**No active flush+fence on this stage.** Bucket scan reads the bucket
cachelines but relies on owner-self being the only writer + x86 MOESI
within the local coherence domain to see fresh data.

#### LWS2 slot_dir_lock (line 571-573)

| # | Site | Code | Intent | Operand domain |
|---|---|---|---|---|
| — | `cxl_directory.h:94-95` (`slot_directory_lock`) | `pthread_spin_lock` | host-local mutex; directory is DRAM (MAP_SHARED) | DRAM |

**No flush+fence.** Directory is host-local DRAM per §I7 / §I8.

#### LWS3 sharer_inval (line 593-613)

| # | Site | Code | Intent | Operand domain |
|---|---|---|---|---|
| — | (none — from worker's view) | `send_invalidate(target, key)` writes DRAM `AggrSlot` (`cxl_kv_ops_A.cc:1084-1102`). Sender thread later emits the CXL InvalRing entry — that flush happens in the sender, not the worker. | — |

**No flush+fence on the worker.** The DRAM AggrSlot atomic spin is
host-local coherent.

#### LWS4 cow_publish (line 622-690) — 3 sub-paths

**DELETE sub-path** (line 624):

| # | Site (src:line) | Code | Intent | Operand domain |
|---|---|---|---|---|
| **LW-F1** | `cxl_kv_ops_A.cc:298` (in `retire_slot`) | `flush_line(slot)` | publish the just-zeroed key so peer hosts see EMPTY | CXL slot |
| **LW-N1** | `cxl_kv_ops_A.cc:299` (in `retire_slot`) | `store_fence()` (sfence) | order clflushopt before any later peer-visible action | barrier |

**Inline u64 sub-path** (line 631, when `pool_ == nullptr`):

| # | Site | Code | Intent | Operand domain |
|---|---|---|---|---|
| **LW-F2** | `cxl_kv_ops_A.cc:292` (in `publish_slot_cow`) | `flush_line(slot)` | publish slot.{value, key} to CXL so peer reads see it | CXL slot |
| **LW-N2** | `cxl_kv_ops_A.cc:293` (in `publish_slot_cow`) | `store_fence()` | order clflushopt | barrier |

**Blockpool sub-path** (line 633-687, the production case):

| # | Site (src:line) | Code | Intent | Operand domain |
|---|---|---|---|---|
| **LW-F3..Fn** | `cxl_kv_blockpool.cc:171` (in `pool->write`) | per-cacheline `flush_line(p)` over (4 + value_len) bytes — 17 cachelines for V=1024 | publish CXL block bytes so peer reads see fresh value | CXL block |
| **LW-N3** | `cxl_kv_blockpool.cc:175` (in `pool->write`) | `store_fence()` | order clflushopts before any later slot publish | barrier |
| **LW-F4** | `cxl_kv_ops_A.cc:292` (in `publish_slot_cow`) | `flush_line(slot)` | publish slot.{encoded, key} | CXL slot |
| **LW-N4** | `cxl_kv_ops_A.cc:293` (in `publish_slot_cow`) | `store_fence()` | order clflushopt | barrier |

#### LWS5 dir_state + unlock (line 693-705)

| # | Site | Code | Intent | Operand domain |
|---|---|---|---|---|
| — | (none) | de->version++, de->state, de->sharer_bitmap writes — DRAM-resident directory; pthread_spin_unlock — host-local | DRAM |

**No flush+fence.** Directory is host-local DRAM.

#### LWS6 own_cache (line 708-738)

| # | Site | Code | Intent | Operand domain |
|---|---|---|---|---|
| — | (none) | `cache_pool_insert` / `cache_pool_evict` / `tls_insert` / `tls_evict` | DRAM-resident | DRAM |

**No flush+fence.**

---

### Summary count

| Path / sub-path | flush_line count | full_fence/sfence count | Conditional gate |
|---|---:|---:|---|
| LR HIT (LRS2H) | 0 | 0 | — |
| LR MISS LRS3 bucket | **2** | **1** | `FUSEE_LR_DEL_OWNER_FLUSH=1` removes all 3 |
| LR MISS LRS3 pool->read | 16 (V=1024) | 1 | unconditional |
| LR MISS LRS4 populate | 0 | 0 | — |
| LW LWS1 slot scan | 0 | 0 | (iter-17A removed) |
| LW LWS2 dir_lock | 0 | 0 | — |
| LW LWS3 sharer_inval (worker) | 0 | 0 | — |
| LW LWS4 retire_slot (DELETE) | 1 | 1 | — |
| LW LWS4 publish_slot_cow (slot) | 1 | 1 | — |
| LW LWS4 pool->write block | 17 (V=1024) | 1 | — |
| LW LWS5 dir_state | 0 | 0 | — |
| LW LWS6 own_cache | 0 | 0 | — |

**Anomaly B–H3** = LR-F1+LR-F2+LR-N1 (the gated 2 flush + 1 fence on
LRS3 bucket scan). Already removed in bnoflush build; iter-19A Phase 2
confirmed 5.9× thpt gain at zipf-1.5.

**Remaining unisolated sites on local_read/write that warrant Task 3c
A/B testing**: 4 group sites (LR-F3+LR-N2 in pool->read, LW-F1+LW-N1 in
retire_slot, LW-F2+LW-N2 + LW-F4+LW-N4 in publish_slot_cow, LW-F3+LW-N3
in pool->write). See Part 2 for theoretical needs and Part 3 for the
A/B plan.

---

## Part 2 — Theoretical analysis under local coherence

**Master question** (from user): are all of these flush+fence
unnecessary because local coherence already handles them?

**Master answer** (preview): **NO — most are still necessary** because
the operands are CXL-resident memory which is OUTSIDE the local
coherence domain by definition. Local coherence is x86 MOESI within
ONE host's caches; CXL memory shared across hosts is not coherent in
hardware. Each site's necessity below.

### Local coherence covers

- Same-host workers (sharing DRAM via MAP_SHARED|MAP_ANONYMOUS)
- Host-local DRAM (cache_pool, TLS, slot directory, AggrSlot)
- Sender thread observing worker's DRAM-resident AggrSlot
- Receiver thread updating local DRAM caches

### Local coherence does NOT cover

- CXL `/dev/dax0.0` mappings shared across hosts (buckets, slot.value,
  blockpool blocks, InvalRing entries, WriteRing entries, …)
- Owner host's writes on these CXL regions reaching peer host's L1/LLC

### Per-site verdict

| Site | What operand is | Who needs to see it | Coherence path | Verdict |
|---|---|---|---|---|
| **LR-F1** flush_line(bucket) on LRS3 | CXL bucket cacheline 1, owner-self read | owner-self load of bucket->slots[s].key/value | **Local coherence DOES cover** owner-self load of owner-self write within the same host. Bucket gets written by execute_write_local (same host), L1 has fresh copy via MOESI. Flush is **NOT required** for correctness. | **REMOVABLE** (already proven by iter-19A Phase 2 bnoflush 5.9× gain) |
| **LR-F2** flush_line(bucket+64) on LRS3 | CXL bucket cacheline 2 | same | same | **REMOVABLE** (same group as LR-F1) |
| **LR-N1** full_fence after LR-F1/F2 | barrier | — | needed only to order the (removed) clflushopts | **REMOVABLE** (LR-F1/F2 gone → fence has nothing to order) |
| **LR-F3..Fn** flush_line per cacheline in pool->read | CXL block bytes | owner-self read of own writes (LWS4 wrote them) | **Local coherence DOES cover** owner-self read of recently-written CXL memory IF the worker that wrote it is on the same host. Owner host is single-writer → all writes by execute_write_local. L1 has fresh copy via MOESI. **NOT required for owner-self read of owner-self written data.** | **CANDIDATE for removal** — needs A/B test (Task 3c) |
| **LR-N2** full_fence after LR-F3..Fn | barrier | — | needed only to order the clflushopts | **REMOVABLE** (group with LR-F3..Fn) |
| **LW-F1** flush_line(slot) in retire_slot (DELETE) | CXL slot (key=0) | **peer hosts** so they observe DELETE | **Local coherence does NOT cover** cross-host visibility. Peer host without an explicit invalidate would still hold the prior key in its L1 cache copy and could service a read with stale value. | **REQUIRED for peer visibility** (cross-host correctness) |
| **LW-N1** store_fence in retire_slot | barrier | — | order LW-F1 before any subsequent peer-observable event | **REQUIRED** as long as LW-F1 stays |
| **LW-F2 / LW-F4** flush_line(slot) in publish_slot_cow | CXL slot (key, value) | **peer hosts** for read coherence | same as LW-F1 — cross-host visibility | **REQUIRED for peer visibility** |
| **LW-N2 / LW-N4** store_fence in publish_slot_cow | barrier | — | order LW-F2/F4 | **REQUIRED** |
| **LW-F3..Fn** per-cacheline flush in pool->write | CXL block bytes | **peer hosts** for cross-host read | same — cross-host visibility | **REQUIRED for peer visibility** |
| **LW-N3** store_fence in pool->write | barrier | — | order LW-F3..Fn | **REQUIRED** |

### Exception — peer hosts also send InvalRing messages

Even for the "REQUIRED for peer visibility" sites: the protocol ALSO
broadcasts InvalRing OP_INVALIDATE to all peer-host sharers (LWS3
broadcast). If a peer-host receiver guarantees an explicit cache evict
+ re-register before any subsequent read, the flush of LW-F1..F4 might
be RECOVERABLE by the InvalRing semantics. But this is only true under
two strong conditions:

1. The InvalRing OP_INVALIDATE reaches the peer BEFORE any peer read of
   the affected key. If a peer just started a read at the moment the
   inval was sent, the peer's read uses its old cache_pool entry and
   the inval arrives AFTER → peer reads stale.
2. Peer's `cache_pool_lookup` always returns from DRAM cache_pool, never
   from a stale L1 copy of the underlying CXL slot. Currently it does
   (cache_pool is DRAM by design), so the slot flushes are mainly for
   peers that fall to the OWNER-SELF-MISS path via forward_read and
   thereby touch the CXL slot directly.

So the "peer-visibility" sites are needed for the corner where a peer's
`forward_read` runs on the owner's behalf and the receiver hand-rolls a
bucket scan (xhost_read path) — those need the flushed bucket / block to
be visible. **This applies cross-host, not same-host owner-self.**

### Refined per-site theoretical needs (final)

| Group | Sites | Needed for local_write/local_read correctness on owner-self? | Needed for cross-host correctness via xhost_read or peer cache_pool_register? |
|---|---|---|---|
| **G1** = LR-F1, LR-F2, LR-N1 | LRS3 bucket pre-scan flush+fence | **NO** (already proven iter-19A B-H3) | NO — peer hosts use forward_read which does its own xhost flush sequence |
| **G2** = LR-F3..Fn, LR-N2 | pool->read flush+fence on local-read MISS | **NO** for owner-self, same MOESI argument as G1 | NO — peer hosts forward_read|
| **G3** = LW-F1, LW-N1 | retire_slot flush+fence on DELETE | NO for owner-self — owner already sees own write via MOESI | **YES** — peers reading via xhost_read need to see the cleared slot to fail-fast (return -1) instead of returning stale value |
| **G4** = LW-F2, LW-N2 | publish_slot_cow flush+fence on inline-u64 publish | NO for owner-self | YES (peers via xhost_read) |
| **G5** = LW-F4, LW-N4 | publish_slot_cow flush+fence on blockpool publish | NO for owner-self | YES (peers via xhost_read) |
| **G6** = LW-F3..Fn, LW-N3 | pool->write flush+fence on block bytes | NO for owner-self | YES (peers reading via forward_read or peer cache_pool_register) |

**Summary**:
- G1 + G2 are removable for both correctness criteria.
- G3 / G4 / G5 / G6 are removable for owner-self correctness, but
  **required for cross-host correctness**. They are not zero-risk
  removals — they trade single-host throughput for cross-host
  staleness windows.

### Why "owner-self MOESI is enough" specifically for LR-F1..F2

Owner-self bucket reads on LRS3 hit the same L1/L2/L3 hierarchy that
`execute_write_local` writes through (`publish_slot_cow` →
`flush_line(slot)` → CXL devdax store). Because clflushopt itself
evicts the cacheline from L1 to LLC and out to memory, the next owner-
self load re-fetches via the normal cache hierarchy and gets fresh
data without any further clflushopt by the reader. The
`FUSEE_LR_DEL_OWNER_FLUSH=1` build proved this empirically (iter-19A
Phase 2 verified 5.9× thpt gain at zipf-1.5 with no observed read-stale
events on owner-self).

### Why "pool->read flush is theoretically removable" for owner-self

By the same MOESI argument: when `execute_write_local` writes a block
via `pool->write` (which itself flushes the block bytes to CXL), and
then a same-host worker calls `pool->read` to fetch back, the owner
host's L1 has either: (a) NOTHING (the writer's clflushopt evicted L1
copies of those lines), in which case the reader re-fetches from LLC /
CXL and gets the fresh bytes anyway, or (b) FRESH bytes from MOESI
propagation of the writer's store. Either way no clflushopt is needed
by the reader.

The owner-self case is the common one because Anomaly A pattern shows
owner-self HIT/MISS dominates on workload-a / workload-c. **Removing
LR-F3..Fn + LR-N2 is the largest theoretical-but-untested gain on the
local_read MISS path** because V=1024 means 16 cachelines × clflushopt
per op — a non-trivial latency on the MISS path.

---

## Part 3 — A/B test plan (Task 3c)

Goal: empirically validate the theoretical analysis above. For each
flush+fence group **except G1** (already isolated in iter-19A Phase 2
as B-H3), run a controlled A/B comparison.

**Base build**: `build-cxl-w1-v1024-bnoflush`
- FUSEE_LR_DEL_OWNER_FLUSH=1 (B-H3 already removed; production-correct)
- `lru_epoch.store` ENABLED (LRU eviction semantics intact)
- LWS probes enabled (`FUSEE_LOCAL_WRITE_PROBE=1`)
- LRS probes enabled (`FUSEE_LOCAL_READ_PROBE=1`)

**Comparison axis**: one isolation per group via new build flag.

| Flag | Removes | New build dir |
|---|---|---|
| `FUSEE_LR_DEL_POOL_READ_FLUSH=1` | G2 (LR-F3..Fn + LR-N2) | `build-cxl-w1-v1024-bnf-G2` |
| `FUSEE_LW_DEL_RETIRE_FLUSH=1` | G3 (LW-F1 + LW-N1) | `build-cxl-w1-v1024-bnf-G3` |
| `FUSEE_LW_DEL_SLOT_PUB_FLUSH=1` | G4 + G5 (LW-F2/F4 + LW-N2/N4) | `build-cxl-w1-v1024-bnf-G45` |
| `FUSEE_LW_DEL_POOL_WRITE_FLUSH=1` | G6 (LW-F3..Fn + LW-N3) | `build-cxl-w1-v1024-bnf-G6` |

**Workload cells** (per build):

| Cell | Workload | T | distribution | KV | cache_pct | reps |
|---|---|---:|---|---:|---:|---:|
| c1 | workload-a (R50 U50) | 64 | zipf-0.99 | 1024 | 1 | 3 |
| c100 | workload-a | 64 | zipf-0.99 | 1024 | 100 | 3 |

**Total**: 4 builds × 2 cells × 3 reps = 24 runs. Plus bnoflush
baseline (2 cells × 3 reps = 6 runs) → 30 runs.

**Notes / caveats** (Task 3c-pending discussion items):

1. **Cross-host correctness validation**: G3-G6 are NOT zero-risk
   removals (Part 2 theory). For each of those isolations, additionally
   run cross-host hash-diff verification (peer-host reads through
   forward_read should still return correct values). If hash-diff fails,
   that flag CANNOT be defaulted ON in production. The thpt number
   stands as an upper bound on the correctness-preserving gain available.

2. **Workload variant**: c1+c100 limited to workload-a. Workload-c
   (100% read) doesn't exercise local_write — the LW-* removals will
   show no signal on workload-c. Recommend running workload-c
   additionally only for the G2 (LR pool->read) isolation.

3. **Probe overhead**: probes add per-op timestamp; if their cost
   masks the flush gain, fall back to a no-probe build of the same
   variant for the headline thpt number.

4. **Build flag wiring**: requires CMake / Makefile edits per new
   flag. Each new flag is a `#if !FUSEE_LW_DEL_xxx`-style gate around
   the existing flush/fence sites. Pattern follows
   FUSEE_LR_DEL_OWNER_FLUSH precedent.

5. **Deferred until user discussion**: per user message
   "等我回来讨论", do NOT launch Task 3c testbed work until user
   reviews Parts 1-2 above. Once approved, the build edits + 30 runs
   take ~6 hours on g1/g2.

---

## Part 4 — Recommendations to user (for Task 3c kickoff discussion)

**Strong-evidence recommendations** (no further testing needed):
- G1 already validated in iter-19A Phase 2 — bnoflush is the right
  default. Ship via RAP in iter-20A.

**Likely-wins, test for confirmation**:
- G2 (pool->read flush in local_read MISS): MOESI argument strong;
  could yield meaningful local-read MISS gain at high T. Recommend
  testing first because it doesn't touch cross-host correctness.

**Trade-off-required, test then think**:
- G3, G4, G5, G6 (all the local_write flushes): theoretical removal
  trades cross-host visibility for owner-self throughput. Need
  hash-diff cross-host validation alongside the perf delta.

**Open question for user**:
- Should we entertain a config that defaults to "flush off, peer-only
  cache_pool register sync via InvalRing" for the cross-host paths?
  This would convert LW-F1..LW-Fn to NO-OPs by relying entirely on
  InvalRing semantics. Larger change, separate iter.
