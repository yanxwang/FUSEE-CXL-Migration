# Task plan — iter-4A — Protocol A v2 (directory-based cache coherence + sharding)

**Author**: Claude
**Date drafted**: 2026-04-29
**Status**: DRAFT — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter4A-shard]`, `[iter4A-dir]`, `[iter4A-cache]`, `[iter4A-blockpool]`,
`[iter4A-msg]`, `[iter4A-write]`, `[iter4A-read]`, `[iter4A-forward]`,
`[iter4A-enforce]`, `[iter4A-sweep]`)

**Spec reference**: `docs/design_goals.md §Protocol A v2` (§I-XIII).
This plan operationalizes that spec into 10 incremental phases.

---

## Plan overview

10 phases, each with:
- **Goal**: 1-2 sentence
- **Code changes**: file-level
- **Validation experiment**: with concrete success criterion
- **Bottleneck check**: what to measure to detect overhead/regression
- **Spec invariant coverage**: which I/AP each phase addresses

The phases are ordered such that each phase produces a runnable
artifact (compiles + passes its own validation) before the next
starts. Phases 1-5 build infrastructure with no behavioral change;
phases 6-8 wire the protocol; phases 9-10 close out.

**Hard constraint**: each phase MUST end with its validation
experiment passing on g3+g4 testbed before next phase starts. No
"will fix later" handoff.

**Phase 0 (already done)**: spec written to `docs/design_goals.md`,
22-scenario cheat sheet in §VI-B, decision card in §VI-A.bis,
enforcement framework in §X.

---

## Phase 1: Sharding table + key-to-host routing infrastructure

**Goal**: Add `ShardingTable` data structure, hash function, and
runtime routing decision; no behavioral change yet (just compute
where ops would go).

**Spec coverage**: I2 (sharding rule), AP10 (no LFM for
writer-writer mutex), §XII O1 (sharding hash function default).

**Code changes**:
- NEW `src/cxl_sharding.h`: `ShardingTable` struct (fixed array,
  read-only after init), `host_of(key)` lookup function
- NEW `src/cxl_sharding.cc`: init from `FUSEE_NUM_HOSTS` env, hash
  function `(fnv1a(key) >> 31) & (H-1)` for $H=2$
- MODIFIED `tests/cxl_ycsb_runner.cc`: per-op compute owner host;
  add counter `cross_host_op_count` (no actual forwarding yet)

**Validation experiment**:
1. Unit test (`tests/sharding_test.cc`):
   - Insert 100k synthetic keys, verify `host_of()` produces ~50/50
     split (within 5% of uniform expectation)
   - Verify same key maps to same host across multiple lookup calls
2. Smoke run on g3+g4: workload A T=4 cache=on, 100k ops:
   - Verify `cross_host_op_count` ≈ 50% of total ops (uniform key)
   - Verify protocol C baseline metric unchanged (sharding code
     compiled in but not yet invoked on the C path)

**Success criterion**: unit test passes; smoke run reports
cross-host fraction in [45%, 55%]; protocol C cell stable median
within ±2% of pre-phase 1 baseline.

**Bottleneck check**: measure `host_of()` latency in the unit test;
must be ≤ 10 ns (single FNV-1a + shift + mask). Anything more
indicates overhead from a non-trivial implementation choice.

---

## Phase 2: Per-slot directory data structure + host-local locks

**Goal**: Allocate `SlotDirectory` in MAP_SHARED DRAM; implement
`spinlock_acquire`/`spinlock_release` over PROCESS_SHARED memory;
no protocol behavior yet (directory empty, never queried).

**Spec coverage**: I5 (per-slot single-level directory),
I7 (directory home in DRAM), I8 (host-local lock, NOT LFM),
AP6/AP7 (anti-patterns), §III layout.

**Code changes**:
- NEW `src/cxl_directory.h`: `SlotDirectoryEntry` struct (16 B),
  `SlotDirectory` 2D array, `directory_init()`, lock/unlock/CRUD
  primitives
- NEW `src/cxl_directory.cc`: MAP_SHARED + MAP_ANONYMOUS allocation
  (sized for `num_buckets × kCxlKvSlotsPerBucket × 16 B`);
  `pthread_spin_init` with PTHREAD_PROCESS_SHARED attribute
- MODIFIED `tests/cxl_ycsb_runner.cc`: allocate directory pre-fork;
  workers inherit via fork; primary inits

**Validation experiment**:
1. Unit test (`tests/directory_test.cc`):
   - Stress test: 8 threads × 10k ops × 64 directory entries,
     concurrent set_sharer / clear_sharer / read_state operations;
     final state matches expected (sequential-equivalent execution).
   - TSan run: no data races detected.
2. Cross-process test: parent fork 4 children, all share MAP_SHARED
   directory, each child does 1k modifications, parent verifies
   final aggregate state.
3. Smoke run on g3+g4: directory allocated but unused on protocol A
   path; verify total VSZ growth matches expected
   `~7 MB × num_workers` per host.

**Success criterion**: unit test 100% pass; TSan reports no race;
fork test final state matches sequential prediction; VSZ growth
within 10% of expected.

**Bottleneck check**: measure spinlock acquire/release uncontended
latency; must be ≤ 30 ns (a few cache hits + atomic CAS). Measure
contended latency at 8 threads on same entry; must scale roughly
linearly with thread count (no pathological back-off).

---

## Phase 3: MAP_SHARED cache pool with lazy stale flag

**Goal**: Allocate per-host `KvCachePool` in MAP_SHARED DRAM;
implement `lookup`, `insert`, `update`, `set_stale`, `evict` ops;
no integration with protocol yet (cache populated by direct API
calls in tests).

**Spec coverage**: I3 (MAP_SHARED across same-host workers),
I4 (lazy stale flag), AP3 (no per-worker private cache),
AP4 (no physical delete on invalidate).

**Code changes**:
- NEW `src/cxl_cache_pool.h`: `KvCacheEntry` (key, value bytes
  pointer, value size, stale flag, LRU epoch), `KvCachePool`
  (hashmap + LRU list)
- NEW `src/cxl_cache_pool.cc`: `MAP_SHARED + MAP_ANONYMOUS`
  allocation; lock-free open-addressing hashmap or per-bucket
  spinlock
- NEW `tests/cache_pool_test.cc`: stress test + race test

**Validation experiment**:
1. Unit test:
   - Insert 1M random KV pairs (each 256 B), verify lookup hit rate
     before any eviction
   - Set stale on random subset, verify lookup of stale entries
     returns `entry && entry->stale == 1` (not a miss; lazy flag
     correctness)
   - LRU eviction: fill cache to capacity + 10%, verify eviction
     touches oldest entries (LRU correctness)
2. Race test (8 threads concurrently insert + lookup + set_stale):
   - TSan: no race
   - Final state matches sequential-equivalent execution
3. Cross-process test: 4 forked workers share cache; one worker's
   `set_stale` propagates to other workers' subsequent `lookup`
   without explicit IPC (verifies hardware coherence on
   MAP_SHARED).

**Success criterion**: 100% test pass; cache hit rate ≥ 99% before
eviction; LRU eviction order verified; TSan clean.

**Bottleneck check**: measure cache hit fast path latency: must be
≤ 100 ns (DRAM hashmap lookup + 1 byte stale check). Measure stale
flag set latency: must be ≤ 20 ns (single atomic byte store).

---

## Phase 4: KV blockpool — per-host partition + CoW write path

**Goal**: Allocate per-owner-host KV blockpool segments on CXL;
host-local DRAM free list metadata; expose `blockpool_alloc()` and
`blockpool_free()` API; not yet wired into write path.

**Spec coverage**: I6 (CoW only), §VI synchronization table
(blockpool free list = host-local spinlock, NOT LFM).

**Code changes**:
- MODIFIED `src/cxl_kv_blockpool.h/cc` (existing file from variable-KV
  iter): partition into `H` segments; per-segment free-list metadata
  in DRAM (not CXL); add per-segment host-local spinlock
- NEW `tests/blockpool_test.cc`: alloc/free stress + cross-host
  isolation test

**Validation experiment**:
1. Unit test:
   - Per-segment alloc/free: 100k cycles in each segment,
     verify no double-alloc
   - Size-class correctness: alloc with various sizes, verify
     returned blocks belong to correct size class
2. Cross-host test on g3+g4:
   - Host A allocs from segment 0 (its own); Host B allocs from
     segment 1; verify their alloc'd addresses don't collide and
     fall in respective segments.
   - Host B never alloc's from segment 0 (verified by counter).
3. CoW write micro-bench:
   - Allocate new block → write value bytes → measure latency
     for size = {64, 256, 1024, 4096} B
   - Verify NT store path used for ≥ 256 B (compare cycles to
     plain+clflushopt for same size; NT should be faster)

**Success criterion**: alloc/free correct under stress; cross-host
isolation verified; CoW write latency within 20% of theoretical
(layer-2 ceiling): 256 B in ~100 ns NT vs ~150 ns plain (relative
ordering correct), 1024 B in ~150 ns NT vs ~600 ns plain (NT 4×
faster), 4096 B in ~600 ns NT vs ~3 µs plain.

**Bottleneck check**: alloc latency ≤ 50 ns uncontended (DRAM
spinlock + free list pop); contended ≤ 200 ns; cross-host
isolation enforces zero conflict.

---

## Phase 5: N:1:1:N message types + ForwardStaging buffer + sender/receiver dispatch

**Goal**: Extend existing `cxl_per_host_ring`/aggregator infrastructure
with 5 message types: `OP_INVALIDATE`, `OP_CACHE_REGISTER`,
`OP_CACHE_EVICT`, `OP_WRITE_FORWARD`, `OP_RESPONSE`. **No inline
value payload anywhere**: messages carry only metadata + CXL pointers
(per spec §XII O3 / §VI-A.bis MESSAGE PAYLOAD POLICY). For
`OP_WRITE_FORWARD`, value bytes are pre-written into a per-forwarder
ForwardStaging buffer on CXL; message carries a pointer.

**Spec coverage**: I11 (cross-host write via N:1:1:N forward,
not LFM), §III (CXL layout includes ForwardStaging[H]), §XII O3
(no inline payload), §VI synchronization rules for SPSC rings.

**Code changes**:
- MODIFIED `src/cxl_per_host_ring.h`: rename `PerHostInvalEntry` →
  `PerHostMessage`; **entry size remains 64 B** (single cacheline,
  preserve false-sharing-defense from iter-2A); add `op_type`,
  `inner_op_type`, `staging_ptr`, `value_size`, `slot_pointer`,
  `status` fields via union by op_type
- MODIFIED `src/cxl_a_local_aggregator.h`: `AggrEntry` carries
  same union fields; **no inline payload bytes** (spec §VI-A.bis
  MESSAGE PAYLOAD POLICY)
- NEW `src/cxl_forward_staging.h/cc`: per-host ForwardStaging buffer
  on CXL; size 1 MB / host (sufficient for $T \times \text{max-kv}=
  86 \times 1\text{KB} \approx 86$ KB of in-flight forwards, with
  10× headroom); host-local DRAM free-list metadata; allocator API
  `forward_staging_alloc(size)`/`forward_staging_free(ptr)`
- MODIFIED `src/cxl_kv_ops_A.cc::sender_loop_k` and
  `receiver_loop_k`: per-op-type counters; receiver's `op_type`
  switch dispatch (placeholder bodies for now: just count + free
  staging on OP_WRITE_FORWARD ack)
- NEW `tests/n11n_message_dispatch_test.cc`: inject each of 5
  message types, verify counter parity on receiver
- NEW `tests/forward_staging_test.cc`: stress alloc/free + cross-host
  read-from-staging correctness (forwarder writes value with NT
  store + sfence; owner-side worker reads via clflushopt+mfence+
  load; verify byte-equal)

**Validation experiment**:
1. Inject 1k of each of 5 message types from g3 → g4; verify g4
   receiver counters all reach exactly 1k
2. ForwardStaging round-trip:
   - Forwarder writes value bytes (size = 256, 512, 1024) to
     staging via NT store + sfence
   - Sends OP_WRITE_FORWARD with staging_ptr
   - Owner reads via clflushopt+mfence+load, verifies value bytes
     byte-equal to forwarder's input
   - Owner sends OP_RESPONSE (status=OK, no value bytes)
   - Forwarder receives ACK, frees staging slot
   - Run 100k iterations; zero byte mismatch; zero staging leak
3. Round-trip latency at value size ∈ {256, 512, 1024} B:
   - p50 ≤ 3 µs, p99 ≤ 6 µs (forward + ACK roundtrip)
4. Throughput: pipeline 100k messages, must be ≥ 3 M msg/sec
   per channel (lower than fully inline because each forward
   adds NT store on forwarder + clflushopt+load on owner;
   layer-3 utilization ~50% expected)

**Success criterion**: counter parity for all 5 op types; 100% byte
equality on ForwardStaging round-trip; latency within budget; zero
staging slot leak after 100k iterations.

**Bottleneck check**: sender batch K=4 yields ≥ 1.5× throughput over
K=1; per-CXL-message bytes = 64 B (single SPSC slot, unchanged).
Forward roundtrip latency at 1 KB ≤ 6 µs (NT 256 ns staging write +
~2 µs message round-trip + ~700 ns owner-side fetch + ~2 µs reverse
ack ≈ 5 µs theoretical).

---

## Phase 6: Writer full integration (owner == self host) — UPDATE first, then INSERT/DELETE

**Goal**: Wire the full owner-self write path: directory spinlock →
alloc + CoW write to CXL → invalidate sharers via N:1:1:N → atomic
CAS slot.pointer → directory state update → cache update. No
cross-host forward yet (only test owner-self ops).

**Spec coverage**: I9 (commit point ordering), I10 (write commit =
all sharer ACK + CXL durable), I6 (CoW), I8 (host-local directory
lock), Scenario 4/5/6 of §VI-B.

**Code changes**:
- NEW `src/cxl_kv_ops_A_v2.h/cc`: new protocol class
  `CxlKvStoreA_v2` implementing `update`/`insert`/`remove` with the
  v2 path
- MODIFIED `src/cxl_kv_store.h`: add `FUSEE_OPT_A_V2 = 4`; default
  remains `FUSEE_OPT_C`
- MODIFIED `tests/cxl_ycsb_runner.cc`: select store impl by
  `CONSENSUS_OPT`
- NEW `tests/protocol_a_v2_correctness_test.cc`: hash-diff battery
  (5 reps × T={2,4,8,16} × workloadA 100K UPDATE, only same-host
  routing — keys whose owner == 0 only, force all writes on g3)

**Validation experiment**:
1. Hash-diff battery (owner-self only):
   - 5 reps × T={2,4,8,16} × 100K UPDATEs = 20 runs
   - Final bucket array byte-identical between hosts → 20/20 PASS
2. Strict-A linearizability micro-test:
   - Host A writes K, host B reads K immediately → must see new value
   - 1000 such write-then-read pairs; zero stale reads observed
3. Latency decomp at T=4 cache=on:
   - Measure each step (spinlock acquire, blockpool alloc, KV write,
     invalidate + ACK wait, CAS, directory update, cache update)
   - Sum within 10% of measured wall time per op (instrumentation
     correctness)

**Success criterion**: 20/20 hash-diff PASS; 0 stale reads in 1000
write-then-read pairs; wall time decomp coverage ≥ 90%.

**Bottleneck check**: per-op latency at T=4 owner-self uncontended:
≤ 5 µs (spinlock 30 ns + alloc 50 ns + KV write 150 ns + invalidate
2 µs + CAS 1 µs + directory 50 ns + cache 50 ns ≈ 3.4 µs theoretical).

---

## Phase 7: Reader cache_register message + slow path

**Goal**: Wire the full read path (fast path + slow path + register
ordering). Cross-host miss now triggers cache_register message →
owner host updates directory → response carries value bytes →
requester populates local cache.

**Spec coverage**: I9 (read fast path no shared atomic load; slow
path = register-then-fill), AP15 (cache fill before register ACK
must abort), Scenario 1/2/3 of §VI-B.

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A_v2.cc`: implement `search()` with both
  paths
- MODIFIED `src/cxl_kv_ops_A_v2.cc::receiver_loop_k`: wire
  `OP_CACHE_REGISTER` handler to update directory + build response
- MODIFIED `src/cxl_directory.cc`: add `register_sharer()` method
  with version increment (for ABA defense in evict path)
- NEW `tests/protocol_a_v2_read_test.cc`: race tests (TSan) for
  register-before-fill ordering

**Validation experiment**:
1. Hash-diff battery extended:
   - 5 reps × T={2,4,8,16} × workload A 100K (50/50 R/W) =
     20 runs; **must include cross-host reads**
   - All reads must return correct value (compared to ground-truth
     dictionary maintained by test framework)
2. Race test (TSan):
   - 100K iterations of: writer on host A updates K, reader on
     host B fetches K
   - TSan must report no data race
   - No reader observes a stale value (every read must show the
     post-write value if the read comes after the write's return)
3. Latency at T=4 cache=on:
   - Read fast path (cache hit): ≤ 100 ns p99
   - Read slow path (cross-host miss): ≤ 3.5 µs p99

**Success criterion**: 20/20 hash-diff PASS; TSan clean; latency
within budget.

**Bottleneck check**: read fast-path latency p50 must be ≤ 60 ns
(otherwise something other than DRAM hashmap is on the path).

---

## Phase 8: Cross-host write forward + cache_evict message

**Goal**: Wire forward path for writes whose owner is a peer host;
wire cache_evict message for LRU pressure.

**Spec coverage**: I11 (cross-host write via N:1:1:N forward, not
LFM), Scenario 7 (forward) and Scenario 8 (cache evict) of §VI-B.

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A_v2.cc::update/insert/remove`: check
  owner; if remote, enqueue OP_WRITE_FORWARD and spin on response
- MODIFIED receiver: wire `OP_WRITE_FORWARD` handler — local
  worker dequeues, executes Phase 6's owner-self write code,
  enqueues response
- MODIFIED `src/cxl_cache_pool.cc`: add LRU eviction trigger;
  eviction path enqueues `OP_CACHE_EVICT` to owner if owner != self
- MODIFIED receiver: wire `OP_CACHE_EVICT` handler

**Validation experiment**:
1. Hash-diff battery, full cross-host:
   - 5 reps × T={2,4,8,16,32} × workload A 100K (50/50 R/W) =
     25 runs
   - Force key distribution so 50% of writes are cross-host
     forwarded
   - All 25 PASS
2. Forward latency:
   - p50 cross-host UPDATE: ≤ 7 µs
   - p99 cross-host UPDATE: ≤ 15 µs
3. Cache evict correctness:
   - Force LRU eviction by limiting cache size; verify
     `cache_evict` reaches owner; verify directory.sharer_bitmap
     reflects the evict
4. Stress soak (10M ops, T=8):
   - 0 hash-diff failures
   - 0 timeouts
   - 0 OOM (cache eviction working)

**Success criterion**: 25/25 hash-diff PASS; latency within budget;
soak completes; cache evicts apply correctly.

**Bottleneck check**: forward roundtrip latency p50 must be in
[5 µs, 8 µs]; outside this range indicates either path elision or
unwanted serialization.

---

## Phase 9: Hard enforcement infrastructure (H1-H4)

**Goal**: Land the four hard enforcement mechanisms from spec §X
so that future violations of the protocol are detected automatically.

**Spec coverage**: §X H1, H2, H3, H4.

**Code changes**:
- H1 asserts in code:
  - `src/cxl_cache_pool.cc::init`: runtime check MAP_SHARED flag
    set; abort if not
  - `src/cxl_directory.h`: typedef `host_local_spinlock_t` distinct
    from `shm_mutex_t` (compile-time barrier)
  - `src/cxl_oplog.cc::begin/commit`: add `[[deprecated]]` +
    runtime abort
  - `src/cxl_kv_ops_A_v2.cc::attach`: AP13 trip-wire (abort if
    routing fields stay default while NUM_HOSTS > 1)
  - `invalidate_sharers()`: AP14 trip-wire (abort if sharers ==
    full bitmap without explicit broadcast flag)
  - reader cache fill API: AP15 trip-wire (require register_acked
    flag)
- H2 NEW `tests/protocol_a_v2_invariant_check.cc`: race tests +
  MAP_SHARED check + AP13 trip-wire test
- H3 MODIFIED `scripts/run_g34_scaling_sweep.sh`: add 5 validation
  gate computations + abort on failure
- H4 NEW `.git/hooks/pre-commit`: regex check on commit message
  for protocol A files

**Validation experiment**:
1. Run `tests/protocol_a_v2_invariant_check.cc`: all 6 invariants
   tested fire correctly on intentional violations.
2. Trip wire test:
   - Manually set `phys_hosts_pr_=1` while NUM_HOSTS=2 → process
     SIGABRT in attach
   - Manually pass `sharers=full_bitmap` to invalidate → SIGABRT
3. Sweep gate test:
   - Run a smoke sweep with `phys_hosts_pr_` patched to default
     (intentional violation); sweep script must abort with G3 fail
4. Pre-commit hook test:
   - Commit a change to `cxl_kv_ops_A_v2.cc` with no I/AP reference
     in message → reject
   - Commit with `[I9 fix race]` reference → accept

**Success criterion**: 6/6 trip wires fire on intentional violation;
sweep gate aborts on G3 fail; pre-commit hook rejects/accepts
correctly.

**Bottleneck check**: H1 runtime asserts must add ≤ 100 ns each to
attach time; H4 hook must complete in < 1 sec.

---

## Phase 10: Full sweep + retrospective + iter-5A teaser

**Goal**: Run full validated sweep matrix; write iter-4A summary
with stage attribution; generate iter-5A candidate list with RAP
analysis.

**Spec coverage**: §IX (G1-G5 all reported), §X P2 (spec drift
audit), §XIII (RAP retrospective for iter-4A design choices).

**Code changes**: docs only.

**Validation experiment**:
1. Full scaling_ycsb sweep:
   - 5 protocols × 5 workloads × 8 T values × 2 cache modes ×
     5 reps = 1000 cells (or scoped down per testbed budget)
   - All 5 validation gates G1-G5 pass for each cell
2. iter-4A summary doc:
   - Headline numbers (5-rep medians) for each (protocol,
     workload, T) cell
   - Stage attribution via Phase 6 latency decomp
   - Spec drift audit: each I/AP traced to a code location
   - RAP retrospective: which Phase 1-9 design RAP attacks were
     verified by sweep data, which were not
3. iter-5A candidate list:
   - ≥ 3 candidate optimizations
   - Each with full RAP analysis (per §XIII format)
   - Verdict + decision (accept/modify/reject) for each

**Success criterion**: 1000/1000 cells valid (or 100% within
scoped subset); summary doc has stage attribution for every
headline number; RAP retrospective for each phase; ≥ 3 iter-5A
candidates with full RAP.

**Bottleneck check**: layer-3 utilization (sweep peak / layer-2
ceiling) per workload reported in summary; targets ≥ 0.5 for
write-heavy workloads (vs C baseline ~0.35-0.59).

---

## Cross-phase verification matrix

| Phase | Validates I/AP | Cumulative test count | Cumulative LOC delta |
|-------|----------------|------------------------|----------------------|
| 1 | I2, AP10 | 2 | ~200 |
| 2 | I5, I7, I8, AP6, AP7 | 5 | ~700 |
| 3 | I3, I4, AP3, AP4 | 8 | ~1200 |
| 4 | I6 | 10 | ~1700 |
| 5 | I7, I11 | 12 | ~2200 |
| 6 | I9, I10, I6, I8 | 14 | ~3000 |
| 7 | I9, AP15 | 16 | ~3300 |
| 8 | I11 | 18 | ~3700 |
| 9 | §X H1-H4 + AP13/14/15 | 22 | ~4100 |
| 10 | §IX G1-G5 | docs | ~4200 |

---

## Phase exit criteria (apply to every phase)

Before moving to phase $n+1$:

1. ✅ All phase $n$ validation experiments pass on g3+g4 testbed
2. ✅ Phase $n$ bottleneck check produces an in-budget number
3. ✅ Phase $n$ commit message references the I/AP it covers
4. ✅ No regression on any prior phase's validation experiment
5. ✅ Spec drift audit (P2) finds no orphan implementation

If any criterion fails, the phase is not done. **No "fix in next
phase" handoff** — that is the iter-3A failure mode.

---

## Out of scope

- Replication / fault tolerance (deferred per §XII O4)
- OpLog write/read in iter-4A (per I12)
- Consistency hashing (per §XII O5; static high-bits is sufficient
  for fixed $H = 2$)
- Variable-host-count dynamic re-shard (future iter when $H$
  becomes dynamic)
- Read-path cache=on collapse for read-heavy workloads (iter-3A
  open issue; carried forward, may surface in Phase 10 retrospective
  as iter-5A candidate)

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Phase 6 hash-diff fails because of subtle race in invalidate→CAS ordering | Med | Phase 6 includes TSan + 1000 write-then-read fixture; will catch ordering bugs before sweep |
| Phase 7 register-before-fill ordering subtle: race window if reader cache fills before owner directory updates | Med | Phase 7 explicit TSan test for ordering; race detected → halt and root cause before integration |
| Phase 8 cross-host forward latency p99 worse than expected at high T (queue backpressure) | Med | Phase 8 latency check at T=8/16/32; if p99 > 20 µs, halt and analyze sender batch K, ack waiting strategy |
| Phase 9 H4 pre-commit hook breaks legitimate commits | Low | Hook regex tested in Phase 9 validation; can be temporarily disabled per-commit if false positive |
| Phase 10 full sweep takes longer than budget | Low-Med | Have phase-budget escape valve: scope to 200 cells (5 × 5 × 4 × 2 × 5/5) if needed; report in summary |
| Phase 6/7 reveals strict-A linearizability bug requiring spec revision | Low | If detected, escalate to user per P3 (spec change goes through user); never silently revise spec to match buggy implementation |

---

## Quick stats

- **10 phases**, each with concrete code + test + experiment
- **~4200 LOC delta** across protocol A v2 + tests
- **22 invariants/AP** covered across phases
- **5 validation gates** (G1-G5) reported in every Phase 10 sweep cell
- **6 hard enforcement trip wires** (Phase 9) prevent future
  silent drift

---

## Phase 0 already-done checklist

- [x] Spec written: `docs/design_goals.md §Protocol A v2` (§I-XIII)
- [x] §VI-A primitive reference table
- [x] §VI-A.bis NT-vs-clflushopt decision rule + decision card
- [x] §VI-B 22-scenario implementation cheat sheet
- [x] §X enforcement framework (H1-H4 + P1-P3)
- [x] §XI PR review checklist
- [x] §XII open questions (O1-O5)
- [x] §XIII Reviewer Attack Process (RAP)
- [x] CLAUDE.md updated with RAP triggers + hard enforcement

---

## Pending user review

Please review and confirm/modify:
1. **Phase ordering**: 10 phases makes sense, or split/merge?
2. **Validation experiment scope**: each phase's experiment cover
   enough? Too much?
3. **Bottleneck check thresholds**: latency budgets reasonable for
   g3+g4 testbed?
4. **Risk register**: missing any Phase-level risk you foresee?
5. **Out of scope**: anything I missed that should be in or
   anything in scope that should be out?

After your confirmation I will start Phase 1.
