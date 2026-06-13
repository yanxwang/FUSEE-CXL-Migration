# iter-21A constraints

**Status**: drafting (2026-06-08)
**Branch context**: feat/cxl-migration
**Drives from**: Protocol F paper-§6.2/§6.3 benchmark completion
(see [docs/protocol_F_vs_FUSEE_alignment.md](../protocol_F_vs_FUSEE_alignment.md)).

iter-21A is **two-phase**:

1. **§1 Read/write path audit + modification** — before benchmarking, confirm
   Protocol A's R/W paths match the intended design.  This section is a
   placeholder; the concrete edits get appended after the design-
   discussion turn with the user (which uses this doc as the running
   reference).
2. **§2 Paper-aligned benchmarks** — once §1 lands, run the SAME three
   experiments on Protocol A that we just ran on Protocol F:
   - **Fig 10** — single-client latency CDF (4 op types × 100K ops)
   - **Fig 11** — multi-client per-op throughput (4 op types ×
     {2,4,6,8,16,32,64,128} clients)
   - **Fig 13** — YCSB-A/B/C/D throughput (4 workloads × same client sweep)

   Output gets cataloged the same way Protocol F did, plus a side-by-side
   F-vs-A comparison appended to the alignment doc.

This is the **final pre-paper benchmark of Protocol A**.  Numbers from
this iter become the "Protocol A baseline" cited in the FUSEE-CXL
migration paper alongside Protocol F.

---

## §1 R/W path audit + modification

This section holds the audit-and-patch list arising from the
design-discussion turn.  The sub-headings are pre-populated with the
items I already mapped while preparing this doc; the discussion turn
will (a) confirm or reject each item, (b) add anything I missed,
(c) decide which patches are in-scope for iter-21A vs deferred.

### 1.1 Current state — read path (LR, XR)

**LR (Local Read, `owner_host(K) == self`)** ([src/cxl_kv_ops_A.cc:2602+](../../src/cxl_kv_ops_A.cc#L2602)):
1. Cache check: `cxl_cache_pool` per-bucket seqlock CAS (iter-10A Phase 2,
   [src/cxl_cache_pool.h:68-77](../../src/cxl_cache_pool.h#L68)).
2. On cache miss: direct CXL bucket scan + pool block read.  **No
   owner-self bucket pre-flush** (iter-19A Phase 3 removed it; MOESI
   keeps owner's L1 coherent, [src/cxl_kv_ops_A.cc:545-547](../../src/cxl_kv_ops_A.cc#L545)).

**XR (Cross-Host Read, `owner_host(K) != self`)** ([src/cxl_kv_ops_A.cc:2052+](../../src/cxl_kv_ops_A.cc#L2052) `forward_read_direct`):
1. Send `OP_CACHE_REGISTER` via ReadRing to owner.
2. Owner's `read_handler` ([src/cxl_kv_ops_A.cc:2300+](../../src/cxl_kv_ops_A.cc#L2300))
   scans its own bucket, **publishes value bytes directly into
   `ReadStagingMatrix[req_host][owner][shard][slot_idx]`** (iter-11A
   Phase 1 "forwarder-pool-direct").
3. Requester polls staging until ready.

### 1.2 Current state — write path (LW, XW)

**LW (Local Write, `owner_host(K) == self`)** ([src/cxl_kv_ops_A.cc:507+](../../src/cxl_kv_ops_A.cc#L507)):
1. Acquire per-(bucket,slot) pthread_spinlock (W2).
2. If UPDATE/DELETE with peer sharers: broadcast `OP_INVALIDATE` to each
   peer via InvalRing (W4-W6, [src/cxl_kv_ops_A.cc:593-600](../../src/cxl_kv_ops_A.cc#L593)).
3. `pool_->alloc()` new block (W7).
4. `pool_->write()` value bytes + per-cacheline clflushopt + sfence (W8).
5. Publish slot pointer (W9, **commit point**).
6. Directory update (W10) — bitmap reset to self-only, unlock.
7. `cache_pool_evict` bumps bucket epoch (W11).

**XW (Cross-Host Write, `owner_host(K) != self`)** ([src/cxl_kv_ops_A.cc:999+](../../src/cxl_kv_ops_A.cc#L999) `forward_write` → `forward_write_direct`):
1. Worker pre-stages value bytes into `ForwardStagingMatrix[self][owner][shard][slot_idx]`.
2. Enqueue WriteEntry to `WriteRing[self][owner][shard]`.
3. Owner's `write_handler` ([src/cxl_kv_ops_A.cc:2225+](../../src/cxl_kv_ops_A.cc#L2225))
   picks up entry, calls `execute_write_local` (same path as LW).
4. Owner stamps `resp_op_id`; writer spins on it via `generic_spin_wait`
   ([src/cxl_kv_ops_A.cc:322-358](../../src/cxl_kv_ops_A.cc#L322)).

### 1.3 Sharding + ownership invariant (current state)

[src/cxl_sharding.cc:7-23](../../src/cxl_sharding.cc#L7):
- `owner_host(key) = (fnv1a_u64(key) >> shift) & mask`
  (high-bit slice, shift = `64 - log2(num_hosts)`).
- `bucket_idx(key) = fnv1a_u64(key) % num_buckets_` — **independent of
  owner_host**.

**Invariant currently held** (by code-flow, not by index-encoding): every
LW on a key K runs on `owner_host(K)`.  XW forwards to owner.  → owner
is the sole writer of K's slot.

**Invariant NOT held** (this is iter-20A's open issue): a single bucket
may contain slots whose owners are different hosts.  Cross-host bucket
visibility requires explicit flush+fence on owner-self writes (iter-19A
Phase 3 audit's G3/G4+G5/G6 flushes still present).

### 1.4 Core iter-21A change — per-host bucket partition

**DECIDED 2026-06-08 (user)**: per-host bucket partition is the
**central iter-21A architectural change**, not a deferred candidate.

**Invariant to enforce**: a bucket can only hold keys belonging to a
single host.  Two keys K1, K2 land in the same bucket ⇒ `owner_host(K1)
== owner_host(K2)`.

**Implementation** (iter-20A plan §1.2 Option A — formula-only, no CXL
layout change):

```cpp
// in CxlKvStoreA, near bucket_idx():
constexpr uint32_t kNumBucketsPerHost = num_buckets_ / num_hosts_;

uint32_t bucket_idx(uint64_t key) const {
  uint32_t owner = owner_host(key);                                  // sharding high-bits
  uint32_t local = fnv1a_u64(key) % kNumBucketsPerHost;              // local-in-segment
  return owner * kNumBucketsPerHost + local;                         // global index
}
```

Bucket array `buckets_[num_buckets]` stays as a single contiguous CXL
region; the index just encodes owner in the high bits.  Each host
"owns" its segment `buckets_[owner * kNumBucketsPerHost ..
(owner+1) * kNumBucketsPerHost)`.

**Constraint**: `num_buckets % num_hosts == 0`.  Assert at attach.
At num_hosts=2 and num_buckets=65536, each host owns 32K buckets.

### 1.5 Consequences — flush/fence removal opportunity

Goal: with bucket ownership now globally enforced, **every bucket access
is owner-self** (peer hosts NEVER scan, read, or write into another
host's bucket segment — cross-host reads go through `read_handler`
running on the owner host, which is MOESI-coherent to other threads
on the same host).

The iter-19A Phase 3 flush+fence audit catalogued these cross-host
visibility flushes on the LW path
([iter19A_flush_fence_audit_v2.md](../iter19A_flush_fence_audit_v2.md));
the partition makes most of them **dead code**:

| Group | Site | Pre-partition reason | Post-partition status |
|---|---|---|---|
| **G3** | `retire_slot()` flush+sfence (DELETE) | Peer that wants to INSERT a key K' with `bucket_idx(K') == bucket_idx(K_deleted)` and `owner_host(K')==peer` must see the slot empty | **REMOVE** — peer never INSERTs into owner's bucket segment after partition. |
| **G4** | `publish_slot_cow()` inline flush+sfence (W9) | Peer with `bucket_idx(K')==bucket_idx(K_just_published)` and `owner_host(K')==peer` must see the slot occupied to avoid duplicate / reuse | **REMOVE** — same reason as G3. |
| **G5** | `publish_slot_cow()` blockpool flush+sfence | Same as G4 + write_handler ACK ordering | **REMOVE bucket portion; KEEP** the pool/staging portion (cross-host data visibility — see G6 row). |
| **XR.R3** | `read_handler` bucket flush+mfence at receiver | Receiver runs on owner host reading owner's own bucket; iter-19A already flagged as MOESI-redundant | **REMOVE** — flagged for removal in iter-19A but never shipped; partition makes it unambiguous. |
| **LR / LW bucket pre-flush** in `search()` / `insert()` / `update()` / `remove()` | Bucket cachelines might have been touched by peer's INSERT into the same bucket (different slot) | **REMOVE** — only owner ever touches owner's bucket cachelines after partition. |
| **Per-slot spinlock acquire flush** (DRAM directory) | Cross-host spinlock visibility (PROCESS_SHARED) | **KEEP** — directory is DRAM-host-local; spinlock cross-host visibility is via MOESI within a host.  Same-host workers use it.  Not affected by partition. |
| **G6** | `pool->write()` per-cacheline flush+sfence | Peer's eventual `pool->read` from HAZARD-mode resp_blk_off reads owner's CXL pool segment directly | **KEEP** in HAZARD/RESERVED.  In STAGING (current default), G6 isn't triggered anyway.  Pool block visibility is orthogonal to bucket ownership. |
| **`forward_write` ring entry flush** | Cross-host WriteRing slot visibility | **KEEP** — ring entries are cross-host (writer publishes, owner consumes). |
| **`forward_read` staging matrix flush** | Cross-host ReadStagingMatrix slot visibility | **KEEP** — staging is cross-host. |

**Estimated impact**: removing G3/G4/G5(bucket)/XR.R3/LR-LW pre-flushes
= 5 cross-host flushes per write op on the hot path.  iter-19A Phase 3
flush-isolation data already showed each cross-host flush costs
~600-1500 ns at CXL fabric crossing.  → ~3-7 µs per WRITE saved on
hot path → 1.5-3× write throughput potential.

### 1.6 Patch list (concrete edits)

This list is the iter-21A scope.  To be expanded in the discussion turn
with line numbers + exact code blocks; for now: the patch sites.

**Phase A — partition formula**:
- [ ] `src/cxl_kv_ops_A.cc` — `bucket_idx()` returns `owner *
  kNumBucketsPerHost + local`.  Locate the existing `bucket_idx()`
  definition and replace.
- [ ] `src/cxl_kv_ops_A.h` (or `.cc`) — expose `kNumBucketsPerHost` as
  a member-derived constant.
- [ ] Attach path — assert `num_buckets % num_hosts == 0`.

**Phase B — hash-diff validation (BEFORE removing any flush)**:
- [ ] Run `tests/cxl_kv_ops_A_*_hashdiff` (whatever exists; otherwise
  port `tests/cxl_kv_ops_F_2host_hashdiff.cc`) with the new bucket_idx
  formula.  Must PASS — no slot collisions, no missed keys.
- [ ] Run iter-19A reference YCSB-A c=16 × 1M trans and compare load
  thpt + trans thpt against the iter-19A baseline.  Must be within
  ±10 %.

**Phase C — flush removal (one group at a time, hash-diff after each)**:
- [ ] G3 (retire_slot) — `src/cxl_kv_ops_A.cc` DELETE path.
- [ ] G4 (publish_slot_cow inline) — `src/cxl_kv_ops_A.cc` LW W9.
- [ ] G5 bucket portion — same site.
- [ ] XR.R3 (read_handler bucket flush) — `src/cxl_kv_ops_A.cc` ~2300.
- [ ] LR / LW bucket pre-flush — find via grep on `flush_line(b1)` /
  `flush_region(buckets_` in `cxl_kv_ops_A.cc`.

After each Phase C removal: hash-diff + YCSB-A c=16 sanity.  If any
regression → revert that one removal + flag for design re-discussion.

**Phase D — micro_throughput / microbench binaries** (after §1.6 gates
pass).

### 1.7 Other candidate edits (decision needed)

Items I flagged during the audit but the user hasn't ruled on yet:

1. **Old-block GC on UPDATE** — audit said "no explicit freelist call
   in shipped code for old blocks, so they leak in the allocation bump
   counter".  At Fig 13 c=128 × 5 s YCSB-A (50 % updates), each worker
   does ~50K-100K updates → 6.4M-12.8M leaked blocks × 1024 B = 6-13
   GB pool growth per cell × 32 cells = >100 GB total.  Options:
   (a) confirm GC is silently active and audit missed it,
   (b) bump pool size to ~200 GB + daxctl reset between cells,
   (c) wire freelist into UPDATE before iter-21A benchmark.
2. **TLS L1 cache deletion** confirmation (iter-20A §2; `src/cxl_tls_cache.cc`
   shows `D` in `git status` — confirm read path is on the no-L1
   branch).
3. **Variable KV size env knob** — find or add the `FUSEE_KV_BLOCK_SIZE`-
   equivalent that flows through to `cxl_kv_blockpool` attach so the
   benchmark binary can request 1024 B records.
4. **Ring sizing at T=128** — concrete numbers for WriteRing per-shard
   capacity + ForwardStagingMatrix `peer_blocks_per_peer_`.  iter-20A
   §2.4 flagged the latter as potentially too small.

### 1.8 Validation gates before §2 benchmark

Sanity checklist that must pass before any sweep starts:

- [ ] `tests/cxl_kv_ops_A_*_test` unit tests all PASS (workstation)
- [ ] `tests/cxl_kv_ops_A_2host_hashdiff` (if exists, else add) PASS on
  g1+g2 with T=4 and T=16
- [ ] `tests/protocol_a_ycsb` c=4 × 1M trans on workload-a PASS, matches
  iter-20A reference numbers within ±10 %
- [ ] Attach time at the chosen num_buckets is < 30 s on g1+g2 (pool +
  ring + cache + directory init)
- [ ] No g2 starvation / pool exhaustion in a c=128 × 5 s sanity run

---

## §2 Paper-aligned benchmarks (Fig 10 / Fig 11 / Fig 13)

These three experiments mirror **exactly** what Protocol F just ran.
Setup details + lessons-learned from F apply.  The Protocol A binary
that lands these is `tests/protocol_a_ycsb.cc` (already exists in the
tree, used by iter-1A through iter-19A sweeps) — but we need to extend
it for Fig 10/11 (today it's YCSB-only) OR add two siblings
`protocol_a_microbench.cc` and `protocol_a_micro_throughput.cc` modeled
after the F counterparts.

### 2.1 Hardware + paper baseline

**Testbed**: g1 + g2 (the default platform from 2026-05-29 onward —
[CLAUDE.md](../../CLAUDE.md) §Hosts).  Both hosts attach the same
shared CXL Type-3 device `/dev/dax0.0` (256 GiB).  Kernel 6.15.0-uintr.

**Paper §6.1 hardware** (for reference, NOT what we're matching
hardware-wise):
- 22 machines on CloudLab APT (5 MN + 17 CN), 56 Gbps ConnectX-3 IB.
- Fig 10/11/13: **16 CN + 2 MN**, **128 clients total** (8 per CN),
  **1 index replica + 2 data replicas**, embedded log built but commit
  skipped.

**Protocol A maps to** (cross-protocol convention):

| Aspect | FUSEE paper | Protocol A on g1/g2 |
|---|---|---|
| Memory tier | 2 RDMA MNs | 1 shared CXL Type-3 device |
| Compute nodes | 16 CN | 2 hosts |
| Total clients | 128 | up to 128 (64 per host) |
| Replication | 1 idx + 2 data | 1 (CXL physical singular) |
| Network | 56 Gbps IB | CXL 2.0 + XConn switch |

### 2.2 KV size — STRICT 1024 B

All three figures use **1024 B KV pairs** (paper §6.3 alignment, decided
2026-06-07 — same as Protocol F).

Implementation requirement:
- `cxl_kv_blockpool` must support per-record size 1024 B
  (in iter-13A through iter-18A, A used `kv8` / `kv256` / `kv512` / `kv1024`
  size classes — already in place; just set `kv1024` for all three).
- Insert/update/search API must accept arbitrary-length value blobs
  (verify A's current API or add an `insert_blob(key, void*, len)`
  variant — Protocol F just landed this same shape).

### 2.3 Fig 10 — single-client latency CDF

**Configuration** (mirroring `/home/yanwang/g1/FUSEE/micro-test/latency_test.cc`):
- 1 active client on g1 (host 0)
- g2 (host 1) attaches as **passive peer** so CXL coordination state is
  set up, then sleeps until killed
- 100K ops per type
- Op order: INSERT → SEARCH → UPDATE → DELETE
- `gettimeofday` µs precision per op
- KV: u64 key + 1016 B synthetic value (record_size = 1024)
- Output: per-op µs dumps to `results/{insert,search,update,delete}_lat-Ap.txt`
- Plot: 4-panel CDF matching FUSEE Fig 10 layout

**Result table to publish** (in alignment doc §7.1):

| op | count | avg | p50 | p90 | p99 | p999 | min | max |
|---|---|---|---|---|---|---|---|---|
| INSERT | 100K | | | | | | | |
| SEARCH | 100K | | | | | | | |
| UPDATE | 100K | | | | | | | |
| DELETE | 100K | | | | | | | |

**Comparison row** to fill: Protocol F same-cell + paper Fig 10 numbers.

### 2.4 Fig 11 — per-op multi-client throughput

**Configuration** (mirroring `/home/yanwang/g1/FUSEE/micro-test/micro_test_multi_client.cc`):
- Pthreads per host: 1, 2, 3, 4, 8, 16, 32, 64 → total clients: 2, 4, 6, 8, 16, 32, 64, 128
- 4 phases per cell, in sequence (FUSEE source `micro_test.cc::run_client` lines 191/206/222/238):

  | Phase | Op   | timer (ms) |
  |---|---|---|
  | 1 | INSERT  | 500  |
  | 2 | SEARCH  | 5000 |
  | 3 | UPDATE  | 5000 |
  | 4 | DELETE  | 500  |

- Pre-load slab: each thread loads its private slab BEFORE Phase 1.
  Slab size needs to be **big enough that DELETE phase is timer-bound,
  not key-cap bound** — Protocol F learned this the hard way; for the
  500 ms DELETE phase at ~80K deletes/s/thread we need **≥ 40K keys per
  thread** in the pre-load slab to NOT run out before the timer.
- Workload partitioning: each thread owns key range
  `[global_id * stride, (global_id+1) * stride)` so concurrent INSERTs
  never collide (FUSEE-paper's `coro_id`-strided ops equivalent).
- Cross-host barrier between phases — must use **per-(barrier_idx,
  host_id) cacheline-isolated cookies** (Protocol F discovered the
  bug where two hosts' cookies on the same cacheline race during
  writeback — see [protocol_F_vs_FUSEE_alignment.md §3.7](../protocol_F_vs_FUSEE_alignment.md#part-3-experimental-alignment)).
- DELETE timer: bumped from 500 ms to 500 ms (paper-spec) but slab
  sized so 500 ms is the binding limit.

**Pool sizing**: kTotalRecords must cover pre-load + INSERT phase
growth + UPDATE churn.  For c=128 × 40K pre-load = 5.12M + INSERT
in-flight + UPDATE 4× churn → suggested 8M records × 1024 B = 8 GB pool.

**LFM lock sizing**: each thread must have a **unique LFM id**.  Protocol
F's mistake was sharing host_id across threads on the same host → race
on `b[host_id]`.  A must NOT repeat this: pass `worker_global_id` and
`total_workers` to the lock_slot call (not `host_id, num_hosts`).
`MAX_HOST_NUM=200` in `cxl_shm_profiling/common.h` supports up to 200
distinct ids — enough for 128 workers.

**Result table to publish** (alignment doc §7.2):

| total clients | INSERT (Mops/s) | SEARCH | UPDATE | DELETE |
|---|---|---|---|---|
| 2 | | | | |
| 4 | | | | |
| 6 | | | | |
| 8 | | | | |
| 16 | | | | |
| 32 | | | | |
| 64 | | | | |
| 128 | | | | |

Plus peak-per-op bar chart (4 bars), comparable to Protocol F's
`fig11_peak_bars.png` style.

### 2.5 Fig 13 — YCSB throughput

**Configuration** (mirroring `/home/yanwang/g1/FUSEE/ycsb-test/ycsb_test_multi_client.cc`):
- Same client sweep as Fig 11: per-host {1, 2, 3, 4, 8, 16, 32, 64} →
  total {2, 4, 6, 8, 16, 32, 64, 128}
- 4 workloads: YCSB-A (R50 U50 Zipf), YCSB-B (R95 U5 Zipf), YCSB-C
  (100R Zipf), YCSB-D (R95 latest-key R + 5I)
- Keyspace: 100K Zipf θ=0.99 (paper §6.3)
- KV size: 1024 B (paper §6.3 strict)
- Workload spec files: reuse `setup/workloads/workload{a,b,c,d}.spec_{load,trans}`
  (already on g1/g2 `/tmp/workloads/` from F sweep)
- Trans phase: **5 s wall-clock per cell** (FUSEE `workload_run_time_=5`
  default)
- Load phase: host-0 thread-0 INSERTs all 100K keys (with 1024 B values)
  before any worker starts trans phase

**Same LFM-per-thread-id, cross-host-barrier, pool-sizing, index-cache-
shard rules as §2.4 apply.**

**Result table to publish** (alignment doc §7.3):

| workload | T=2 | T=4 | T=6 | T=8 | T=16 | T=32 | T=64 | T=128 |
|---|---|---|---|---|---|---|---|---|
| YCSB-A | | | | | | | | |
| YCSB-B | | | | | | | | |
| YCSB-C | | | | | | | | |
| YCSB-D | | | | | | | | |

Plus peak-per-workload bar chart + combined line+bar PNG (matching
F-side `fig13_combined.png` style).

### 2.6 Protocol-A-specific concurrency audit

The F-bugs (LFM thread-id collision, global cache mutex, cacheline-shared
barrier cookies) don't apply to A because A is architected differently:

- A uses **sharding + single-owner-write** ([src/cxl_sharding.cc:7-23](../../src/cxl_sharding.cc#L7))
  to partition writes across hosts.  Cross-host writes route via WriteRing
  to the owner host's WriteReceiver, which then runs the local-write path.
  No cross-host mutex.
- A's same-host write serialization is a **per-(bucket, slot)
  `pthread_spinlock`** in DRAM ([src/cxl_directory.h:37-46](../../src/cxl_directory.h#L37)),
  not an LFM.  PTHREAD_PROCESS_SHARED, so the fork-based runner model
  already in `tests/protocol_a_ycsb.cc` works.
- A's DRAM cache (`cxl_cache_pool`) uses **per-bucket seqlock CAS**
  (iter-10A Phase 2, [src/cxl_cache_pool.h:68-77](../../src/cxl_cache_pool.h#L68))
  — readers don't block on writers.  This is NOT the global-mutex F had.

What still needs to be audited for the multi-client benchmark:

| Concern | Where to verify | Why |
|---|---|---|
| **WriteRing slot capacity at T=128** | [src/cxl_write_ring.h](../../src/cxl_write_ring.h), `actual_shards = ceil(T/N)` formula (`FUSEE_RING_SHARDS_FACTOR`) | At 128 workers × 2 hosts × Zipf-0.99 hot key, peer-to-peer write traffic may saturate a single ring shard.  iter-17A added per-shard rings — check the sizing math for T=128. |
| **ForwardStagingMatrix sizing** | [src/cxl_forward_staging.h](../../src/cxl_forward_staging.h), `peer_blocks_per_peer_` | Per-peer write-staging slots used to pre-stage value bytes before WriteRing enqueue.  iter-20A §2.4 flagged: "current value might be too small for 2M trans ops, causing exhaustion."  Worth a sanity check before launching Fig 13 sweep. |
| **KvCachePool capacity** | [src/cxl_cache_pool.h](../../src/cxl_cache_pool.h) + `FUSEE_CACHE_BUCKETS` env | iter-20A §3 ships default 16384 (1 % cache, "mixed regime"); iter-19A data validated 3.3× cluster gain at this default.  Need to make sure this is actually the value at iter-21A run-time. |
| **Cross-host barrier cookie layout** | NEW code added for Fig 10/11 binaries | Whatever barrier mechanism we add for the multi-client Fig 11 sweep MUST follow the F lesson: each (barrier_idx, host_id) on its own cacheline.  The shared-cacheline race kills cross-host sync. |

### 2.7 Existing benchmark binary — what's there vs what's needed

[tests/protocol_a_ycsb.cc](../../tests/protocol_a_ycsb.cc) already
supports:
- 2-host fork-based multi-process model (forks `num_threads - 1`
  children, each pinned to a core)
- Per-worker `global_id` (host_id × num_threads + client_id),
  registered into A's `g_aggr_worker_id` thread_local
  ([src/cxl_kv_ops_A.cc:936-941](../../src/cxl_kv_ops_A.cc#L936))
- YCSB-style trans-file replay (load file + trans file, ops sliced
  modulo total workers)
- Per-worker latency capture + aggregated p50/p99 output
- Multi-rep + cache-on/off env knobs

**Fig 13 fits the existing binary** — it IS a YCSB trans-file benchmark.
Just need an iter-21A sweep driver script.

**Fig 10 and Fig 11 do NOT fit the existing binary**.  They need the
FUSEE-paper 4-phase timed pattern:
- Pre-load slab (each thread inserts kKeysPerClient keys)
- Phase 1 INSERT (500 ms timer, NEW keys past pre-load range)
- Phase 2 SEARCH (5000 ms timer, cycle over pre-loaded keys)
- Phase 3 UPDATE (5000 ms timer, cycle)
- Phase 4 DELETE (500 ms timer, delete from pre-loaded slab)

Plus Fig 10 needs single-client latency CDF (100K ops, gettimeofday µs).

Two new binaries to add (parallel to the Protocol F counterparts):
- `tests/protocol_a_microbench.cc` — single-client latency CDF (Fig 10)
- `tests/protocol_a_micro_throughput.cc` — multi-client 4-phase
  throughput (Fig 11)

Both reuse the existing `CxlKvStoreA` public API; the 4-phase scaffold
is the new code.  No protocol changes needed.

### 2.8 Result archival convention

Match Protocol F:
- Fig 10: `docs/protocol_A_fig10_1024B_<ts>/` with raw `*_lat-Ap.txt`,
  `microbench_summary.csv`, `microbench_cdf.png`, `microbench_summary.md`.
- Fig 11: `docs/protocol_A_fig11_<ts>/` with `summary.csv`,
  `fig11_thpt_vs_clients.png`, `fig11_peak_bars.png`,
  `fig11_combined.png`, `fig11_summary.md`, `fig11_peak_summary.md`.
- Fig 13: `docs/protocol_A_fig13_<ts>/` with `summary.csv`,
  `fig13_thpt_vs_clients.png`, `fig13_combined.png`, `fig13_summary.md`.

Sweep drivers parallel to:
- [scripts/run_protocol_F_fig11_sweep.sh](../../scripts/run_protocol_F_fig11_sweep.sh)
- [scripts/run_protocol_F_fig13_sweep.sh](../../scripts/run_protocol_F_fig13_sweep.sh)
- [scripts/run_protocol_F_fig11_delete_only.sh](../../scripts/run_protocol_F_fig11_delete_only.sh)

(DELETE-only sweep with `kKeysPerClient=40000` + `kTotalRecords=6M`
pool — same fix that unblocked F's DELETE numbers.)

### 2.9 Comparison output

After §2 completes, append to [docs/protocol_F_vs_FUSEE_alignment.md](../protocol_F_vs_FUSEE_alignment.md)
or create a parallel `docs/protocol_A_vs_FUSEE_alignment.md` with the
same §1 (op pseudocode side-by-side) + §7 (3 baselines) layout, AND a
new §X with **side-by-side A-vs-F-vs-paper** for each baseline.

---

## §3 Execution ordering

1. **§1.4 — per-host bucket partition** (Phase A of §1.6 patch list):
   land the `bucket_idx()` formula change + assert at attach.
2. **§1.6 Phase B — hash-diff validation** before any flush removal.
3. **§1.6 Phase C — flush removal, one group at a time**, hash-diff +
   YCSB-A c=16 sanity after each removal.  Roll back any regression.
4. **§1.7 candidate edits** that the discussion turn rules ARE
   in-scope (old-block GC, TLS cache confirmation, KV-size env, ring
   sizing).
5. **§2 audit items** (§2.6 ring/staging/cache verification — no code
   changes if numbers check out).
6. **§2 binaries** — port `protocol_a_microbench.cc` (Fig 10) and
   `protocol_a_micro_throughput.cc` (Fig 11) from F counterparts.  Gate:
   c=2 smoke per binary passes on g1+g2.
7. **Fig 10 + Fig 11 + Fig 13 sweeps**.
8. **Plot + write up** results.  Side-by-side A-vs-F-vs-paper appended
   to the alignment doc.

---

## §4 Out-of-scope for iter-21A

- Fig 12 (KV-size sweep at 256/512/1024 B) — defer to iter-22A if time.
- Fig 14 (num-MN sweep) — CXL has 1 physical MN, not portable.
- New optimizations on the protocol — iter-21A is BENCHMARK-FIRST.  Any
  perf improvements seen in the data become iter-22A backlog, not
  iter-21A scope creep.
- Latency-CDF for multi-client (Fig 10 paper variant at T>1) — not in
  the paper's Fig 10; skip unless §1 discussion adds it.

---

## §5 Open questions (for the design-discussion turn)

Updated 2026-06-08 after reading the architecture blueprint + code.
Items still open:

1. **DECIDED 2026-06-08 — per-host bucket partition is iter-21A core
   scope** (moved to §1.4 / §1.5 / §1.6 above).  The §2 benchmark runs
   AFTER the partition + flush-removal lands and §1.8 validation gates
   pass.

2. **Old-block GC**.  Audit said "no explicit freelist call in shipped
   code for old blocks".  At Fig 13 c=128 × 5 s YCSB-A (50 % updates),
   each worker does ~50K-100K updates → 6.4M-12.8M leaked blocks in 5 s
   × 1024 B = 6-13 GB pool growth per cell.  Multiplied by 32 cells in
   the sweep, that's >100 GB total.  Either (a) confirm GC is silently
   active and I missed it, (b) bump pool size to 200 GB and accept
   leakage per cell + daxctl reset between cells, (c) actually wire
   freelist into UPDATE before iter-21A benchmark.

3. **Variable KV size env knob**.  Where does the existing
   `protocol_a_ycsb.cc` runner pick `block_size`?  Confirm there's a
   single env var (or CLI arg) to set 1024 B for the paper benchmark,
   and that the value flows through to `cxl_kv_blockpool` attach.

4. **WriteRing / ForwardStagingMatrix dimensions at T=128**.  Concrete
   numbers needed: how many bytes is the staging matrix at H=2,
   N_workers=128, kv_size=1024?  How many slots per ring shard?  Plus
   the iter-20A §2.4 flag about `peer_blocks_per_peer_` exhaustion.

5. **Per-(bucket,slot) spinlock count + init time** at our likely
   num_buckets (e.g., 32K or 65K from Fig 13 sweep tradition).  This
   is the A analog of F's LFM-init bottleneck — but spinlock is much
   smaller (16 B vs 38 KB), so probably a non-issue.  Just confirm
   attach time.

6. **The DRAM cache_pool default `FUSEE_CACHE_BUCKETS`**.  iter-19A
   data validated 16384 (1 % cache, mixed regime).  iter-20A §3 ships
   that as the default.  Re-confirm it's wired and not silently
   reverted.

7. **Workstation-runnable hash-diff** — `tests/cxl_kv_ops_A_*` test
   surface.  What's there for multi-thread + multi-host validation?
   Needed for §1.6 validation gates.

8. **Phase-skip / kKeysPerClient knob for Fig 11 DELETE**.  Once we
   write `protocol_a_micro_throughput.cc`, expose the same env-vars F
   has (`FUSEE_F_KEYS_PER_CLIENT`, `FUSEE_F_*_MS`) so the DELETE-only
   re-sweep trick works for A too.
