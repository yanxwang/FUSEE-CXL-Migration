# Task plan — iter-4A — Protocol A (directory-based cache coherence + sharding)

**Author**: Claude
**Drafted**: 2026-04-29
**Revised**: 2026-05-02 — REPLAN after first iter-4A attempt silently
drifted from spec (Phase 7 OP_CACHE_REGISTER never wired, hash-diff
oracle missed §I9 race, only 4-cell sweep ran).
**Status**: REVISED PLAN — supersedes the original 10-phase plan.
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter4A-blueprint]`, `[iter4A-shard]`, `[iter4A-dir]`,
`[iter4A-cache]`, `[iter4A-blockpool]`, `[iter4A-msg]`,
`[iter4A-atomic-audit]`, `[iter4A-write]`, `[iter4A-read]`,
`[iter4A-forward]`, `[iter4A-enforce]`, `[iter4A-sweep]`)

**Spec reference**: `docs/design_goals.md §Protocol A (§I-XIII)`. The
protocol described in §I-XIII *is* Protocol A; there is no `A_v2`
rename. iter-1A through iter-3A's prior implementations of A are
historical milestones that this iter supersedes.

---

## Why this is a REPLAN

The original 10-phase plan delivered all 10 phases on 2026-04-30,
but a 2026-05-01 audit found three structural failures:

1. **Phase 7 OP_CACHE_REGISTER never wired** — cross-host reader
   does direct CXL bucket scan + lazy `cache_pool_insert`, never
   sends register message. As a result `sharer_bitmap` stays at
   `1<<host_id_` forever; writer's invalidate broadcast list is
   empty; concurrent reader+writer sees stale value. **§I9 strict-A
   linearizability silently violated.**

2. **Hash-diff oracle insufficient for §I9**. Both Phase 6 and
   Phase 7 tests use "all writes finish, then cmp" semantics — no
   concurrent reader during writes. The §I9 race window is invisible
   to the oracle, so the missing register code passed CI.

3. **Phase 10 sweep was 4 cells × 1 rep**, violating
   `docs/scaling_ycsb_spec.md` §13 iter-completion gate (80 cells ×
   5 reps required).

**Root cause of (1) and (2)**: Phase 5 declared "5 message types
defined" but only wired counter-stub receivers. Phase 6/7/8 each
needed the message channel for their protocol step but Phase 5 left
the actual handlers as TODOs. Phase 6 wrote `// TODO Phase 7/8` for
invalidate; Phase 7 wrote `// Phase 7 wires that` for register;
Phase 8 didn't go back. Each phase's own oracle (hash-diff) didn't
test the cross-phase invariant, so the chain of TODOs accumulated
silently.

**Root cause of (3)**: Iter-execution-discipline lapse — see
`CLAUDE.md` "cautionary precedent: iter-2A". Same failure mode.

This REPLAN structurally prevents all three:

- **NEW Phase 0.5** writes 12 fail-first invariant oracles BEFORE
  any protocol code lands. Each subsequent phase's exit criterion
  is "oracle X/Y/Z must flip RED→GREEN, prior GREEN must not
  regress." Hash-diff stays as one oracle among many; §I9 race
  oracle is separate and explicit.
- **EXPANDED Phase 5** wires all 5 receiver handlers for real (not
  stubs) before any protocol behavior phase. Phase 6/7/8 only
  attach the sender side at the KV API entry points. No phase ever
  writes `// TODO future phase`.
- **NEW Phase 5.5** systematically audits every CXL-resident atomic
  for the `flush_line + sfence` pair (the bug class behind the
  Phase 10 ring->tail issue that lost 16,000× throughput).
- **Phase 10** strictly gates on §13 (80 cells × 5 reps +
  `gap_to_target.md` + iter summary cites the sweep timestamp).

---

## Plan overview

**13 phases** (Phase 5 is split into 5a + 5b per Q4 decision below).
Each phase has:

- **Goal** (1-2 sentences)
- **Code changes** (file-level)
- **Oracles affected** (which of the 12 invariant oracles + which
  unit/integration tests this phase must turn from RED to GREEN,
  and which prior GREEN oracles must not regress)
- **Bottleneck check** (latency/throughput budget for the phase, to
  catch regressions early)

Phase ordering principle: infrastructure with no protocol behavior
first (1-4); message channel + atomic audit fully ready (5, 5.5);
protocol behavior wired all at once on a complete channel (6-9);
enforcement + sweep close out (9-10).

---

## Phase 0.5 — Invariant→Oracle blueprint + oracle harness

**Goal**: Land an `oracle_harness` test framework + **13** fail-first
invariant oracles. After this phase the `protocol_a_invariants`
binary exists and reports 14/14 RED. Every subsequent phase's
acceptance criterion is "the oracles I touched flipped to GREEN and
the GREEN ones didn't regress."

**Spec coverage**: every I/AP gets a fail-first oracle so silent
drift becomes mechanically impossible (§X H2 reality-check).

**Code changes**:

- NEW `tests/oracle_harness.h` (~200 LOC):
  ```cpp
  // Oracle registration + runner for protocol A invariants.
  //
  //   ORACLE_DEFINE(i9_concurrent_read_write_no_stale,
  //                 /* host_count */ 2,
  //                 /* needs_tsan */ true,
  //                 /* timeout_s */ 30) {
  //     auto coord = oracle_cross_host_coord();
  //     if (coord.is_host(0)) { ... } else { ... }
  //     coord.barrier("after-populate");
  //     ORACLE_REQUIRE(reader_observed == new_value);
  //   }
  struct OracleSpec {
    const char *name;
    int host_count;          // 1 = same-host fork; 2 = cross g3+g4
    bool needs_tsan;
    int timeout_s;
    void (*fn)();
  };
  void oracle_register(const OracleSpec &);
  int  oracle_run_all(int argc, char **argv);

  // Cross-host coordination: barrier + send/recv via the CXL
  // shared-stats region, ordered by FUSEE_RUN_COOKIE.
  struct OracleCrossHostCoord {
    bool is_host(int h) const;
    void barrier(const char *tag);
    void publish(const char *tag, const void *buf, size_t len);
    bool fetch(const char *tag, void *buf, size_t cap);
  };
  OracleCrossHostCoord oracle_cross_host_coord();

  // Same-host fork helper for I3 / cache MAP_SHARED tests.
  int  oracle_fork_workers(int n, void (*body)(int worker_id));

  // Crash-recovery fixture for o_i1 / o_i10. Self-contained — does
  // NOT reuse crash-recover-test/ (which is RDMA-era client/server
  // architecture and doesn't fit unified-node iter-4A model).
  // Requires env FUSEE_SSH_HOST_<id> = "g3" / "g4" + ssh-keys set
  // up so non-interactive ssh works.
  int kill_peer(int host_id, const char *pgrep_pattern);
  int restart_peer(int host_id, const char *binary_path,
                   const char *args, const char *cookie);

  // Perf-counter helper for o_ap16 macroscopic check. Wraps
  // `perf stat -e uncore_imc/cas_count_write/` over a callable;
  // returns dax-region PCIe write count.
  uint64_t measure_pcie_writes_to_dax(std::function<void()> fn);
  uint64_t measure_pcie_reads_from_dax(std::function<void()> fn);

  // Failure macro that records location + dumps SUMMARY before abort.
  #define ORACLE_REQUIRE(cond) ...
  ```
  Backed by a 2 MiB region at the end of the CXL mapping (reuses
  scaling_ycsb's shared-stats convention; orchestrator-supplied
  `FUSEE_RUN_COOKIE`).

- NEW `tests/protocol_a_invariants.cc` (~470 LOC, 14 oracle bodies):

  | Oracle | Spec ref | host_count | tsan | What it tests |
  |--------|----------|-----------|------|---------------|
  | `o_i1_recover_from_cxl` | §I1 | 2 | no | host A inserts 1k keys, hard-kills its process via `kill_peer`; `restart_peer` re-attaches; host B's reader fetches all 1k from CXL byte-equal |
  | `o_i2_owner_routing_uniform` | §I2 | 1 | no | 100k synthetic keys → owner_host distribution within 5 % of uniform; same key hashes to same owner across calls |
  | `o_i2_xhost_writes_use_forward` | §I2/I11 | 2 | no | 1000 cross-host writes; counter `forward_to_owner_calls == 1000`, `execute_write_local_xhost == 0` |
  | `o_i3_cache_map_shared` | §I3 + **AP3** | 1 | no | 4 fork workers; worker 0 set_stale; worker 1 lookup sees `stale==1` without IPC. **Also covers AP3** ("no per-worker private cache") — if a worker has private cache, worker 1 wouldn't observe worker 0's set_stale and oracle FAILs |
  | `o_i4_lazy_not_physical` | §I4 + AP4 | 1 | no | invalidate K then lookup K → returns entry pointer (not NULL) with `stale==1` |
  | `o_i5_per_slot_no_false_share` | §I5/I7 | 1 | no | 16 same-host workers write 1M ops to distinct slots in same bucket; per-thread tput within 30 % of single-thread tput (no contention) |
  | `o_i8_no_lfm_in_protocol_a` | §I8 + **AP10** | 2 | no | run 5k ops mixed; LFM `acquire_count == 0`; spinlock `acquire_count > 0`. **Also covers AP10** ("no LFM for cross-host writer mutex") — if any LFM acquire happens on protocol-A path, oracle FAILs |
  | **`o_i9_concurrent_read_write_no_stale`** | **§I9 race** | **2** | **yes** | **host A reads K (populates cache); host B updates K; host A reads K again — MUST see new value. Run 1000 iterations. This oracle is the one iter-4A's first attempt missed.** |
  | **`o_i9_fast_path_no_cxl_access`** (new) | **§I9 perf** | 1 | no | NEW. 100k cache-hit reads; `measure_pcie_reads_from_dax(fn)` ≤ 1k (allow cache miss + register refresh, but not per-op CXL access); p50 latency ≤ 60 ns. Catches accidental fall-through to CXL atomic during fast path |
  | `o_i10_writer_durable_after_return` | §I10 | 2 | no | host A writes K, returns; `kill_peer(0)` immediately; host B reads K from CXL — must see new value (commit point includes CXL durable + sharer ACK before return) |
  | `o_i11_xhost_forward_observable` | §I11 | 2 | no | forwarder publishes ForwardRing tail via fetch_add; owner host sees new tail within 100 µs (verifies CXL atomic flush plumbing end-to-end) |
  | `o_i12_no_oplog_calls` | §I12 | 1 | no | run 1k ops; `oplog_begin_count == 0`, `oplog_commit_count == 0` |
  | `o_ap16_cxl_atomic_flush_paired` | §VII AP16 (new) | 1 | no | **Macroscopic stress**: harness runs 1M atomic stores against a CXL-resident ring; `measure_pcie_writes_to_dax(fn)` returns count; oracle PASS iff ratio ∈ [0.95M, 1.05M]. Black-box check, no per-instruction inspection (avoids compiler-reorder false positives). Microscopic per-line check is the Phase 5.5 audit table, not this oracle |
  | **`o_kv_blockpool_integrated`** (new, per Q6) | §I6 + Phase 6 | 2 | no | After 1k writes at each of 3 sizes (256/512/1024), verify (a) `slot.block_ptr` always non-NULL CXL address; (b) `blockpool_alloc_count` == ops; (c) no slot.block_ptr equals an inline value pattern (e.g., low 56 bits don't look like a u64 value); (d) reading the block via clflushopt+load returns byte-equal value. Catches "protocol path silently uses inline u64 instead of blockpool" |

  **Anti-pattern coverage map** (to avoid redundant oracles):
  - AP3 → covered by `o_i3_cache_map_shared`
  - AP4 → covered by `o_i4_lazy_not_physical`
  - AP10 → covered by `o_i8_no_lfm_in_protocol_a`
  - AP13/14/15 → enforced via H1 trip wires (Phase 9 SIGABRT in
    debug build), not oracle-tested
  - AP16 → covered by `o_ap16_cxl_atomic_flush_paired` + Phase 5.5
    audit doc

- NEW `tests/CMakeLists.txt` entry:
  `add_executable(protocol_a_invariants ...)` plus a TSan variant
  `protocol_a_invariants_tsan` for oracles with `needs_tsan=true`.

- NEW `scripts/run_invariant_oracles.sh`: cross-host driver that
  ssh-launches the binary on g3+g4 with matching `FUSEE_RUN_COOKIE`
  and aggregates per-oracle PASS/FAIL.

**Validation experiment**:

1. Build `protocol_a_invariants` and `protocol_a_invariants_tsan`.
2. Run on g3+g4: `bash scripts/run_invariant_oracles.sh`.
3. **Expected output**: `14 oracles, 0 PASS, 14 FAIL`. Every
   oracle fails because no protocol code exists yet. This is the
   GREEN condition for Phase 0.5: ALL RED.

**Success criterion**: `protocol_a_invariants` binary builds; oracle
runner driver works (cross-host barrier, fork helper, kill/restart
fixture, perf-counter helper, timeout, TSan build); `14/14 FAIL`
reported with each failure citing a clean spec reference. NO oracle
accidentally PASSing (a green oracle now means the test is
misimplemented — abort and fix the oracle).

**Bottleneck check**: harness overhead per oracle ≤ 50 ms idle
(barrier + setup); cross-host barrier latency ≤ 5 ms p99.

**Why this phase exists**: silent drift in iter-4A's first attempt
was mechanically possible because no oracle tested §I9 race
directly. With this phase, every later phase's claim of "I
implemented X" must be cashed against an oracle. If the oracle is
missing, the implementation can't be claimed.

---

## Phase 1 — Sharding table + key-to-host routing infrastructure

**Goal**: `ShardingTable` data structure + hash function + runtime
routing decision. No protocol behavior change.

**Spec coverage**: I2, AP10, §XII O1.

**Code changes**:
- NEW `src/cxl_sharding.{h,cc}`: `ShardingTable`,
  `host_of(key) = (fnv1a(key) >> 31) & (H-1)`, init from
  `FUSEE_NUM_HOSTS`
- MODIFIED `tests/cxl_ycsb_runner.cc`: per-op compute owner host;
  add counter `cross_host_op_count` (no actual forwarding)

**Oracles affected**:
- `o_i2_owner_routing_uniform`: RED → GREEN
- All other oracles: stay RED (no protocol-A path yet)

**Bottleneck check**: `host_of()` ≤ 10 ns.

---

## Phase 2 — Per-slot directory + host-local spinlock

**Goal**: `SlotDirectory` in MAP_SHARED DRAM; `pthread_spinlock_t`
with `PTHREAD_PROCESS_SHARED`; no protocol behavior yet.

**Spec coverage**: I5, I7, I8, AP6, AP7, §III.

**Code changes**: NEW `src/cxl_directory.{h,cc}` with the structures
and lock primitives.

**Oracles affected**:
- `o_i5_per_slot_no_false_share`: RED → GREEN (directory is
  allocated and laid out cacheline-correctly even though no
  protocol reads/writes it yet; oracle confirms the layout itself
  doesn't false-share against bucket entries)

**Bottleneck check**: spinlock acquire/release ≤ 30 ns uncontended;
8-thread same-entry contended ≤ 250 ns.

---

## Phase 3 — MAP_SHARED cache pool + lazy stale flag

**Goal**: `KvCachePool` in MAP_SHARED DRAM; lookup/insert/update/
set_stale/evict ops; no protocol integration.

**Spec coverage**: I3, I4, AP3, AP4.

**Code changes**: NEW `src/cxl_cache_pool.{h,cc}`.

**Oracles affected**:
- `o_i3_cache_map_shared`: RED → GREEN
- `o_i4_lazy_not_physical`: RED → GREEN

**Bottleneck check**: cache hit ≤ 100 ns; `set_stale` ≤ 20 ns.

---

## Phase 4 — KV blockpool: per-host partition + size-class CoW alloc API

**Goal**: per-owner-host blockpool segments on CXL with **3 size
classes (256 / 512 / 1024 B)**; host-local DRAM free-list per size
class; `blockpool_alloc(size_class)` / `blockpool_free()` API
exposed. API will be wired into the protocol path in Phase 6 (per
Q6 decision below — Variable-KV integration is in-scope for
iter-4A).

**Spec coverage**: I6, §VI synchronization, §III blockpool layout.

**Code changes**:

- MODIFIED `src/cxl_kv_blockpool.{h,cc}`:
  - Partition CXL region into `H` segments (one per owner host).
  - Each segment further partitioned into 3 size-class arenas:
    256 B / 512 B / 1024 B blocks.
  - Capacity per host: `kBlocksPerSizeClass × (256 + 512 + 1024) B`.
    Sized for `num_buckets × kSlotsPerBucket × 3` plus 2× headroom
    for in-flight CoW (old-block-alive-while-new-block-published).
    Default: `2 GB / host` total → ~1 M blocks per size class.
  - Per-(segment, size_class) host-local DRAM free-list with its
    own pthread_spinlock.
  - `blockpool_alloc(size_class) → cxl_block_ptr` (returns CXL
    pointer; block content is uninitialized).
  - `blockpool_free(cxl_block_ptr)`: caller must call after CoW
    commit + invalidate ACK confirms no reader has the old block.

- NEW `src/cxl_kv_block.h`: in-block layout
  ```cpp
  struct CxlKvBlockHeader {
    uint64_t key;          // for fingerprint cross-check
    uint16_t value_size;   // 256 / 512 / 1024
    uint8_t  size_class;   // 0/1/2
    uint8_t  reserved[5];
    // value bytes follow, length = value_size
  };
  static_assert(sizeof(CxlKvBlockHeader) == 16);
  ```
  Total block = header (16 B) + value bytes (256/512/1024).

- MODIFIED `tests/blockpool_test.cc`:
  - 3 size classes × 100k alloc/free cycles each, no double-alloc.
  - Cross-host isolation per size class.
  - Round-trip: alloc → NT-store value → flush → peer-host
    clflushopt+load → byte-equal at all 3 sizes.

**Oracles affected**: none flip yet (Phase 6 wires the protocol
path; this phase is API-only).

**Bottleneck check**: alloc latency ≤ 60 ns uncontended per size
class (DRAM free-list pop); contended ≤ 250 ns; cross-host
isolation strict.

---

## Phase 5 (split) — Message channel **fully wired** (all 5 message types + receivers)

Original Phase 5 had ~1150 LOC across types + ForwardStaging +
receivers + tests, exceeding the 800 LOC review-friendly threshold.
Per Q4 user decision (2026-05-02), Phase 5 is **predetermined to
split** into 5a (metadata-only messages) and 5b (value-carrying
messages). 5a's invalidate/register/evict are simpler
metadata-update handlers; 5b's write-forward + response require
ForwardStaging + cross-cacheline value transfer and benefit from
isolation.

### Phase 5a — metadata-only messages (invalidate / register / evict)

**Goal**: land 3 of 5 message types with full receiver logic; no
value-carrying messages yet.

**Spec coverage**: I9 read side (OP_CACHE_REGISTER), I9 invalidate
(OP_INVALIDATE), §VI sync rules.

**Code changes**:

- MODIFIED `src/cxl_per_host_ring.h`: rename `PerHostInvalEntry` →
  `PerHostMessage`; entry size stays 64 B (preserve cacheline
  discipline); union-by-`op_type` with fields for op_type ∈
  {OP_INVALIDATE, OP_CACHE_REGISTER, OP_CACHE_EVICT, OP_RESPONSE};
  reserve op_type values for OP_WRITE_FORWARD (Phase 5b).
- MODIFIED `src/cxl_kv_ops_A.{h,cc}` (refactored from the old
  `_v2` filename via a separate cleanup commit at the start of
  Phase 5a) — add `receiver_loop_metadata()`:
  - **OP_INVALIDATE handler**: take owner-side directory lock for
    target slot; `cache_pool_set_stale(key)`; clear `sharer_bitmap`
    bit for sender host; release; reply OP_RESPONSE.
  - **OP_CACHE_REGISTER handler**: take directory lock; set
    `sharer_bitmap` bit for sender; bump `version`; copy current
    value pointer (CXL pointer, no value bytes inline); release;
    reply OP_RESPONSE with pointer.
  - **OP_CACHE_EVICT handler**: take directory lock; clear
    `sharer_bitmap` bit; if bitmap == 0 set state INVALID; release;
    reply OP_RESPONSE.
  - **OP_RESPONSE handler**: sender side; mark request slot ready.

- NEW `tests/n11n_metadata_dispatch_test.cc`: for each of 3 op
  types (invalidate, register, evict), inject 1k from g3 → g4,
  verify (a) g4 receiver counter == 1k, (b) g4 directory state
  changed correctly per op type, (c) g3 receives matching
  OP_RESPONSE within timeout.

**Oracles affected**:
- `o_i11_xhost_forward_observable`: RED → GREEN at the message-
  channel level (forward semantic at KV API still RED; flips to
  full GREEN at Phase 8). The oracle measures CXL atomic
  visibility, which is exercised by all 4 message types here.

**Bottleneck check**: message round-trip p50 ≤ 3 µs.

### Phase 5b — value-carrying messages (write_forward + ForwardStaging)

**Goal**: land OP_WRITE_FORWARD + ForwardStaging buffer
infrastructure; receiver-side handler complete; sender-side
plumbing not yet attached to KV API (that's Phase 8).

**Spec coverage**: I11, §III ForwardStaging, §XII O3 no inline
payload, §VI-A.bis MESSAGE PAYLOAD POLICY.

**Code changes**:

- NEW `src/cxl_forward_staging.{h,cc}`: per-host ForwardStaging
  buffer on CXL (1 MB / host); host-local DRAM free-list metadata;
  alloc/free API.
- MODIFIED `src/cxl_per_host_ring.h`: activate OP_WRITE_FORWARD
  union variant (reserved in 5a) — `staging_ptr`, `value_size`,
  `inner_op_type` fields.
- MODIFIED `src/cxl_kv_ops_A.cc::receiver_loop_metadata()` →
  `receiver_loop_full()`:
  - **OP_WRITE_FORWARD handler**: fetch value from forwarder's
    ForwardStaging via `clflushopt + mfence + load`; call shared
    `execute_write_local()` (introduced as a static helper, used
    by both owner-self and forward paths); reply OP_RESPONSE
    (status only, no value bytes per §VI-A.bis MESSAGE PAYLOAD
    POLICY).

- NEW `tests/forward_staging_roundtrip_test.cc`: 100k roundtrips
  at value sizes {256, 512, 1024} B; zero byte mismatch; zero
  staging slot leak; p99 ≤ 6 µs.
- NEW `tests/n11n_write_forward_dispatch_test.cc`: 1k
  OP_WRITE_FORWARD from g3 → g4 with random value bytes; g4
  receiver-side `execute_write_local()` produces correct
  byte-equal state; g3 receives OP_RESPONSE.

**Oracles affected**: none flip yet at the protocol level (the
sender side isn't wired to KV API until Phase 8). The 5a-flipped
`o_i11_xhost_forward_observable` must stay GREEN.

**Bottleneck check**: write-forward roundtrip p50 ≤ 6 µs at 256 B
value; ForwardStaging alloc latency ≤ 80 ns uncontended.

**Why Phase 5 split was predetermined**: per Q4 user decision
2026-05-02. Predetermined split eliminates the "decide at runtime
based on LOC" judgment call (which iter-2A taught us tends to
default to "don't split" once you're already mid-stream). 5a + 5b
also give two natural commit/PR boundaries for review.

---

## Phase 5.5 — CXL atomic flush audit pass

**Goal**: every CXL-resident atomic in the codebase has a
`flush_line(&atomic) + sfence` pair after every store/RMW. Audit
report committed.

**Spec coverage**: NEW AP16 added to `design_goals.md §VII` —
"std::atomic resident in a CXL-mapped region MUST be followed by
`flush_line + sfence` after every store/RMW; consumers MUST
`flush_line + full_fence + load`. Reason: CXL Type-3 is not in the
CPU coherence domain; std::atomic instructions update CPU cache
only, never the device. Precedent: iter-4A first-attempt Phase 10
ring->tail bug, ~16,000× perf loss."

**Two-layer defense against AP16** (per Q3 user decision 2026-05-02).
Single-layer "≤ 5 instructions between atomic store and flush_line"
checks were rejected as too brittle (compiler reorders, inlining
varies by `-O` level, false-positive prone). Replaced with:

- **Macroscopic black-box oracle** (Layer 1, runtime): the
  `o_ap16_cxl_atomic_flush_paired` oracle from Phase 0.5. Stresses
  1M `atomic.store()` against a CXL-resident ring under
  `perf stat -e uncore_imc/cas_count_write/`; PASS iff PCIe write
  count ∈ [0.95M, 1.05M]. Black-box, no per-instruction inspection,
  no compiler-reorder false positives.
- **Microscopic manual audit table** (Layer 2, review-time):
  `docs/iters/iter4A_cxl_atomic_audit.md` lists every CXL-resident
  atomic site for human reviewer sign-off.

**Code changes**:

- NEW `docs/iters/iter4A_cxl_atomic_audit.md`: table of every
  `std::atomic` / `fetch_add` / `compare_exchange*` / `.store(` /
  `.load(` in `src/cxl_*.{h,cc}`, classified:
  - **DRAM-resident** (cache pool, directory): no flush needed.
  - **CXL-resident** (rings, ForwardStaging metadata, anything in
    `/dev/dax0.0` region): must have flush_line + sfence pair.
  - Each row: `file:line | resident | producer flush OK | consumer
    flush OK | fix commit | reviewer sign-off`.

- NEW `src/perf_counter_helper.{h,cc}`: thin wrapper around
  `perf_event_open(2)` + `uncore_imc/cas_count_{read,write}/` to
  measure dax-region PCIe traffic over a callable; used by both
  the `o_ap16` oracle and the `o_i9_fast_path_no_cxl_access`
  oracle.

- ADD AP16 entry to `design_goals.md §VII` and to
  `src/cxl_kv_ops_A.h` H1 trip wire (debug-build runtime).

**Oracles affected**:
- `o_ap16_cxl_atomic_flush_paired`: RED → GREEN (only after audit
  table is 100 % OK AND macroscopic stress oracle passes).

**Bottleneck check**: perf-counter helper overhead per measurement
≤ 50 µs (one syscall + counter read); release build has zero
overhead (helper is conditionally compiled into oracle binary
only).

---

## Phase 6 — Writer owner-self full integration **with blockpool CoW**

**Goal**: wire the owner-self write path on the complete message
channel from Phase 5, **with full Variable-KV blockpool integration
(per Q6 user decision 2026-05-02)**. Slot stores a CXL block
pointer + size, NOT an inline 8 B value. Sequence: directory
spinlock → blockpool_alloc(size_class) → NT-store value bytes into
new block → flush + sfence → send OP_INVALIDATE to every sharer →
wait OP_RESPONSE → atomic CAS slot.pointer = new block → update
directory state → update local cache → **schedule old block for
free** (only after invalidate ACK confirms no reader holds it).

**Spec coverage**: I9 (write side), I10, I6 (CoW), I8 (host-local
lock), §VI-A.bis NT-store decision rule for ≥ 256 B blocks.

**Code changes**:

- MODIFIED `src/cxl_hashtable.h`: `CxlKvSlot` layout change:
  ```cpp
  struct CxlKvSlot {
    uint64_t key;                  // for fingerprint match
    uint64_t block_ptr;            // CXL pointer (0 = empty/deleted)
                                   // Low 4 bits encode size_class
                                   // (3 classes fit in 2 bits; 2
                                   // bits reserved for future flags).
                                   // Block size derivable from class.
  };
  static_assert(sizeof(CxlKvSlot) == 16);
  ```
  No more inline-u64 path. **Hard break from iter-3A's
  `CxlKvSlot { key:u64, value:u64 }`.**

- NEW `src/cxl_kv_ops_A.cc::execute_write_local(key, value_bytes,
  value_size, op_kind)`:
  ```
  spin_lock(directory[slot])
  size_class  = size_class_of(value_size)        // round up
  new_block   = blockpool_alloc(size_class)      // own host segment
  // Format block: header (16 B) + value bytes (NT-store ≥ 256 B)
  CxlKvBlockHeader hdr = { key, value_size, size_class, 0 };
  nt_memcpy(new_block, &hdr, sizeof hdr)         // 16 B header
  nt_memcpy(new_block + 16, value_bytes, value_size)
  sfence()                                        // CXL durable
  // Invalidate sharers (Phase 5a receivers handle the actual
  // directory + cache_pool_set_stale on owner side)
  for each h in directory[slot].sharer_bitmap \ {self}:
    enqueue OP_INVALIDATE(slot, key) → h
  wait all OP_RESPONSE
  // Commit point: atomic CAS slot.pointer
  old_block = atomic_cas(slot.block_ptr, prev, new_block | size_class)
  flush_line(slot); sfence();                    // CXL durable
  directory[slot].state         = Shared
  directory[slot].sharer_bitmap = (1 << self)
  directory[slot].version++
  spin_unlock
  cache_pool_insert(key, value_bytes, value_size)
  // Defer-free: queue old_block to a per-host retirement list;
  // freed lazily after a "no in-flight reader" epoch barrier
  retire_block(old_block)
  ```

- NEW `src/cxl_kv_retire.{h,cc}`: simple epoch-based block retire.
  Workers tick a per-host epoch counter; a block is freed when
  the global min-epoch passes the epoch at which the block was
  retired. Avoids freeing a block while a reader still holds the
  old `slot.block_ptr`.

- MODIFIED `tests/cxl_ycsb_runner.cc`: select Protocol A via
  `CONSENSUS_OPT_A`; runner now passes value-bytes pointer +
  value_size to insert/update (not u64); reads return value bytes.

**Oracles affected**:

- `o_i10_writer_durable_after_return`: RED → GREEN (only if
  invalidate ACK wait + CXL block durable are both before
  return).
- `o_i9_concurrent_read_write_no_stale`: stays RED until Phase 7
  wires register-then-fill on read side.
- **`o_kv_blockpool_integrated` (NEW oracle)**: RED → GREEN.
  After 1k writes, slot.block_ptr is non-NULL CXL address;
  blockpool alloc counter == ops; no slot stores value bytes
  inline (slot's low-bits ≠ value pattern).
- Hash-diff battery extended to 3 size classes:
  - 5 reps × T={2,4,8,16} × **3 sizes (256/512/1024)** × 100K
    UPDATEs using workload-split-by-owner = 60 runs.
  - Final bucket+block array byte-identical between hosts → 60/60
    PASS.

**Bottleneck check**: per-op latency at T=4 owner-self uncontended
≤ 6 µs at kv=256, ≤ 8 µs at kv=1024 (NT-store dominant time
~60 ns/256 B → ~250 ns/1024 B; rest of pipeline ~5 µs).

---

## Phase 7 — Reader register-then-fill (slow path) — **THE FIX**

**Goal**: wire `search()` slow path: on cross-host miss, send
OP_CACHE_REGISTER to owner; receive **(block_ptr, value_size)** via
OP_RESPONSE; **fetch value bytes from CXL block via clflushopt +
mfence + load**; THEN populate local cache. Phase 5 receiver
already updates owner directory's `sharer_bitmap`. Phase 7 is the
sender side + value-fetch.

**Spec coverage**: I9 (read side, register-then-fill), AP15,
§VI-A.bis MESSAGE PAYLOAD POLICY (response carries pointer not
value bytes).

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A.cc::search()`:
  ```
  // Fast path: local cache hit
  if cache_hit(key) and not stale:
    return cached value_bytes        // no CXL access
  owner = host_of(key)
  if owner == self:
    // Same-host miss: scan local bucket, dereference block_ptr
    block = scan_bucket_for_key(key) // CXL bucket + block load
    cache_pool_insert(key, block.value_bytes, block.value_size)
    return value_bytes
  // Cross-host miss
  enqueue OP_CACHE_REGISTER(key) → owner
  spin OP_RESPONSE                     // receiver_loop_metadata on owner
                                       // adds self to sharer_bitmap and
                                       // returns (block_ptr, value_size)
  flush_line(block_ptr); mfence();
  value_bytes = load_block_value(block_ptr, value_size)  // CXL OOB fetch
  cache_pool_insert(key, value_bytes, value_size)        // AFTER register ACK
  return value_bytes
  ```
- AP15 trip wire: assert that `cache_pool_insert(key, ...)` is
  preceded by `register_acked(key)` flag set; abort otherwise.
  Implemented as a debug-build `cache_pool_insert_after_register()`
  wrapper.
- MODIFIED Phase 5a OP_CACHE_REGISTER receiver handler (slight
  extension): response payload now carries `(block_ptr,
  value_size)` not `(value_pointer, 0)`. Block content is OOB on
  CXL — receiver does NOT include value bytes in the 64 B
  PerHostMessage (NO inline payload, per §VI-A.bis).

**Oracles affected**:
- `o_i9_concurrent_read_write_no_stale`: RED → GREEN. **This is
  the oracle iter-4A's first attempt skipped.** It must be GREEN
  at this phase or Phase 7 is not complete.
- `o_i9_fast_path_no_cxl_access`: RED → GREEN. Validates that the
  read fast path (cache hit) does NOT fall through to any CXL
  atomic load. Measured by `measure_pcie_reads_from_dax(fn)` ≤ 1k
  during 100k cache-hit reads. Catches accidental fall-through
  (e.g., reading `directory.version` per op).

**Bottleneck check**: read fast path ≤ 60 ns p50; cross-host miss
(register round-trip) ≤ 4 µs p99.

---

## Phase 8 — Cross-host write forward + cache evict

**Goal**: enqueue OP_WRITE_FORWARD via ForwardStaging (sized for
256/512/1024 B values) when `owner != self`; spin on OP_RESPONSE.
Wire LRU pressure → enqueue OP_CACHE_EVICT to owner.

**Spec coverage**: I11, AP14, §VI-A.bis NO inline payload.

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A.cc::{update,insert,remove}`:
  ```
  if owner_host(key) != self:
    staging = forward_staging_alloc(value_size)   // 256/512/1024
    nt_memcpy(staging, value_bytes, value_size)
    sfence()
    msg.staging_ptr = staging
    msg.value_size  = value_size
    msg.inner_op    = op_kind                     // INSERT/UPDATE/DELETE
    enqueue OP_WRITE_FORWARD(msg) → owner
    spin OP_RESPONSE                              // owner runs Phase 6's
                                                  // execute_write_local
                                                  // (which now allocates a
                                                  // CXL block on owner's
                                                  // segment, NOT in
                                                  // staging — staging is
                                                  // only the message-time
                                                  // value buffer)
    forward_staging_free(staging)
  ```
- MODIFIED Phase 5b OP_WRITE_FORWARD handler (slight extension):
  receiver fetches value bytes from forwarder's staging via
  `clflushopt + mfence + load`, **then calls Phase 6's full
  `execute_write_local()` with those bytes**. So forwarded writes
  end up in owner's blockpool segment via the same CoW path —
  staging is purely a transient transport buffer, never the
  durable home of a value.
- MODIFIED `src/cxl_forward_staging.{h,cc}`: capacity at 1 MB / host
  is sufficient for `T(86) × max_kv(1024) × outstanding(2) ≈
  176 KB`; keep 1 MB for headroom. Add `forward_staging_alloc()`
  size-class-aware (allocates from same 256/512/1024 free-lists).
- MODIFIED `src/cxl_cache_pool.cc`: LRU eviction trigger; if
  evicted entry's owner != self, enqueue OP_CACHE_EVICT.
- REMOVED `kPhase6FilterUnownedOps` build flag.

**Oracles affected**:
- `o_i2_xhost_writes_use_forward`: RED → GREEN.
- `o_i11_xhost_forward_observable` (KV-API level): RED → GREEN.
- `o_i9_concurrent_read_write_no_stale` extended: now also passes
  with cross-host writers (host A reads via cache; host B writes
  cross-host forwarded; host A's cache invalidated by Phase 5
  receiver; host A re-read sees new value).
- `o_i1_recover_from_cxl`: RED → GREEN (full write path now
  durably commits to CXL before returning; restart-and-read works).

**Bottleneck check**: forward roundtrip p50 ∈ [5 µs, 8 µs].

---

## Phase 9 — Hard enforcement (H1-H4)

**Goal**: the four hard-enforcement mechanisms from spec §X land,
plus AP16 (CXL atomic flush) added to runtime check set.

**Spec coverage**: §X H1, H2, H3, H4, AP13/14/15/16.

**Code changes**:
- H1 runtime asserts in `cxl_cache_pool.cc::init`,
  `cxl_directory.h` typedef separation, `cxl_oplog.cc`
  deprecation, `cxl_kv_ops_A.cc::attach` AP13 trip wire,
  `invalidate_sharers()` AP14 trip wire,
  `cache_pool_insert` AP15 trip wire,
  CXL atomic AP16 instrumentation (debug build).
- H2 NEW `tests/protocol_a_invariant_check.cc` — wraps
  `protocol_a_invariants` for CI consumption (smaller subset for
  per-PR CI; full set for nightly).
- H3 MODIFIED `scripts/run_g34_scaling_sweep.sh`: G1-G5 gate
  computations; abort on fail.
- H4 NEW `scripts/git-hooks/{pre-commit,commit-msg}`: regex
  enforcement on commits touching protocol-A files; setup
  documented in `README.md` ("Required git hooks":
  `git config core.hooksPath scripts/git-hooks`).

**Oracles affected**:
- `o_ap16_cxl_atomic_flush_paired`: GREEN (already true since
  Phase 5.5; this phase makes the runtime check production-grade).
- All 12 oracles must be GREEN at this phase's exit. If any is
  RED, Phase 9 is not complete.

**Bottleneck check**: H1 runtime asserts add ≤ 100 ns at attach;
H4 hooks complete in < 1 s.

---

## Phase 10 — Full sweep + summary + iter-5A teaser

**Goal**: run the full 80-cell × 5-rep sweep per
`docs/scaling_ycsb_spec.md` §13; produce required artifacts; write
iter-4A summary; produce iter-5A candidate list with full RAP per
`design_goals.md §XIII`.

**Spec coverage**: §IX validation gates G1-G5; §X P2 spec drift
audit; §XIII RAP retrospective; scaling spec §13 iter-completion
gate.

**Code changes**: docs only.

**Validation experiment**:

1. Full sweep (per Q6: KV size is now a sweep dimension):
   - 1 protocol × 5 workloads × 8 T × 2 cache × **3 KV sizes
     (256/512/1024)** × 5 reps = **1200 SUMMARY.log lines** (= 240
     unique cells × 5 reps).
   - Output to `docs/g34_scaling_ycsb_<timestamp>/`.
   - All 5 G1-G5 gates pass each cell.
   - Wall clock: 3-4.5 h on g3+g4 (3× the old 80-cell budget).
   - Requires `docs/scaling_ycsb_spec.md` to be updated FIRST to
     declare `KV_SIZES="256 512 1024"` and the 1200-line gate
     (companion commit at start of Phase 10).

2. Required artifacts in the timestamped directory:
   - `SUMMARY.log` (with `rep=<r>` and `kv_size=<256|512|1024>`
     fields).
   - `plot_commit.txt`.
   - **15** × `A_thpt_workload<a..f>_kv<256|512|1024>.png`
     (5-rep median; one plot per workload × KV size).
   - 15 × `*_band.png` (min/max band).
   - up to 30 × `A_lat_workload*_kv*_{read,write}.png`.
   - `cache_off/` with the same plot set (another 15 + 15 + 30).
   - `extra/A_target_workload*_kv*.png` (15; gap-to-20-Mops/s
     line per workload × KV size).
   - `extra/A_kv_size_compare_workload*.png` (5; KV-size scaling
     overlay per workload).
   - `extra/A_scaling_efficiency.png`.
   - `summary_table.md`.
   - **`gap_to_target.md`** (auto-generated from SUMMARY.log;
     reports gap to 20 Mops/s for YCSB-A and YCSB-C at each KV
     size).
   - new row in `docs/scaling_ycsb_runs_index.md`.

3. iter-4A summary doc `docs/iters/iter4A_summary_<ts>.md`:
   - 5-rep median headline numbers per (workload, KV size, peak T).
   - Stage attribution via Phase 6 latency decomp at kv=256 / 1024.
   - Spec drift audit (P2): each I/AP traced to a code location
     with grep evidence; 14/14 oracle GREEN evidence pasted.
   - RAP retrospective: which Phase 1-9 design choices were
     verified by sweep data, which were not.
   - Cite the sweep `<timestamp>` per scaling spec §13.

4. iter-5A candidate list (≥ 3 candidates, each with full RAP per
   §XIII).

**Success criterion (HARD GATE)**:

- 1200/1200 SUMMARY.log lines ≠ FAIL.
- 14/14 oracles GREEN.
- All required plot files present (script-checked).
- `gap_to_target.md` exists and shows gap to 20 Mops/s for both
  YCSB-A and YCSB-C.
- iter summary cites the sweep timestamp.

**iter cannot be marked COMPLETE without ALL of the above.** Same
gate as `docs/scaling_ycsb_spec.md §13`.

---

## Cross-phase oracle status table

The single source of truth for "which phase brings which oracle to
GREEN":

| Oracle | P0.5 | P1 | P2 | P3 | P4 | P5a | P5b | P5.5 | P6 | P7 | P8 | P9 | P10 |
|--------|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `o_i1_recover_from_cxl` | R | R | R | R | R | R | R | R | R | R | **G** | G | G |
| `o_i2_owner_routing_uniform` | R | **G** | G | G | G | G | G | G | G | G | G | G | G |
| `o_i2_xhost_writes_use_forward` | R | R | R | R | R | R | R | R | R | R | **G** | G | G |
| `o_i3_cache_map_shared` | R | R | R | **G** | G | G | G | G | G | G | G | G | G |
| `o_i4_lazy_not_physical` | R | R | R | **G** | G | G | G | G | G | G | G | G | G |
| `o_i5_per_slot_no_false_share` | R | R | **G** | G | G | G | G | G | G | G | G | G | G |
| `o_i8_no_lfm_in_protocol_a` | R | R | R | R | R | R | R | R | **G** | G | G | G | G |
| **`o_i9_concurrent_read_write_no_stale`** | R | R | R | R | R | R | R | R | R | **G** | G | G | G |
| **`o_i9_fast_path_no_cxl_access`** | R | R | R | R | R | R | R | R | R | **G** | G | G | G |
| `o_i10_writer_durable_after_return` | R | R | R | R | R | R | R | R | **G** | G | G | G | G |
| `o_i11_xhost_forward_observable` | R | R | R | R | R | **G** | G | G | G | G | G | G | G |
| `o_i12_no_oplog_calls` | R | R | R | R | R | R | R | R | **G** | G | G | G | G |
| `o_ap16_cxl_atomic_flush_paired` | R | R | R | R | R | R | R | **G** | G | G | G | G | G |
| **`o_kv_blockpool_integrated`** | R | R | R | R | R | R | R | R | **G** | G | G | G | G |

R = expected RED, G = expected GREEN, **bold** = the phase that
flips this oracle. A phase is incomplete if its bold cells are not
GREEN, OR if any earlier-bold cell regressed to RED.

---

## Phase exit criteria (apply to every phase)

Before moving to phase $n+1$:

1. ✅ All oracle bold-cells in the column for this phase have
   flipped to GREEN.
2. ✅ All earlier-column GREEN oracles still GREEN (no regression).
3. ✅ Phase bottleneck check produces an in-budget number on g3+g4.
4. ✅ Phase commit message references the I/AP it covers
   (H4 hook check, even if hook not yet installed in early phases —
   manual self-check).
5. ✅ Spec drift audit (P2) finds no orphan implementation:
   no new `// TODO future phase` comments in protocol-A files.

If any criterion fails, the phase is not done. **No "fix in next
phase" handoff** — that is the iter-3A AND iter-4A first-attempt
failure mode.

---

## Out of scope (deferred to iter-5A or later)

- Replication / fault tolerance (per §XII O4).
- K-shard responder threads (forward path scaling). iter-4A's
  single responder is the scale ceiling; iter-5A first task.
- BucketLockTable removal from Protocol A's region (saves ~2.5 GB).
- Read-path cache=on collapse for read-heavy workloads (iter-3A
  open issue carried forward).

(Variable-KV blockpool integration was previously listed here as
out-of-scope but moved IN-scope per Q6 user decision 2026-05-02.)

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Oracle harness cross-host coordination flaky on g3/g4 | Med | Phase 0.5 explicit barrier latency budget (5 ms p99); if exceeded, fall back to file-based barrier on local NFS rather than CXL shared region |
| `o_i9` race oracle hits TSan false positives on cross-host atomics | Med | TSan only used for same-host portions of the test; cross-host correctness verified by value-equality, not TSan |
| Phase 5 receiver scope expansion blows up the phase to too-many-LOC | Med | If receiver code > ~800 LOC, split Phase 5 into 5a (invalidate + register + evict) and 5b (write-forward + response). Decision deferred to start of Phase 5 |
| Phase 5.5 atomic audit finds atomics in iter-3A code paths still in tree | Med-High | Audit table records "needs fix" rows; fix in same phase before advancing. Do NOT skip with "iter-3A code is legacy" — protocol C still uses some of those code paths |
| Phase 10 sweep takes longer than budget (3-4.5 h with kv-size dim) | Med | Per-cell timeout 600 s + cell-skip with user approval recorded in summary; all 240 cells must report some result (timeout or value), not "skipped". If wall-clock exceeds 6 h, escalate before truncating |
| Phase 10 reveals 14/14 oracles GREEN but throughput still very low (forward path bottleneck) | High | This is *expected* — iter-4A's goal is correctness, not the 20 Mops/s target. iter-5A K-shard responder is the throughput follow-up. Phase 10 must still report the gap honestly in `gap_to_target.md` |
| Variable-KV CoW path triggers blockpool fragmentation under sustained workload | Med | Phase 4 sizes 2 GB / host with 2× headroom; epoch-based defer-free in Phase 6 keeps in-flight blocks bounded. If observed in Phase 10 sweep, falls back to size-class compaction in iter-5A |
| `o_kv_blockpool_integrated` heuristic (low-bits ≠ value pattern) yields false negatives on degenerate values like all-zero blocks | Med | Oracle additionally checks `blockpool_alloc_count == ops_count` and reads back the block via clflushopt+load; both must hold. Pure structural check still sound |

---

## Quick stats

- **13 phases** (was 10 in first attempt): adds Phase 0.5 (oracle
  blueprint), Phase 5 split into 5a + 5b (predetermined per Q4),
  Phase 5.5 (atomic audit).
- **~5680 LOC delta** (was ~4200): adds ~630 LOC oracle harness +
  14 oracle bodies + perf-counter helper; ~700 LOC Phase 5 expansion
  (full receivers across 5a + 5b).
- **14 invariant oracles** (was 0 invariant-specific tests, only
  hash-diff). Hash-diff retained as defensive cross-check.
- **1200-line sweep** (240 unique cells × 5 reps; 3× larger than
  the 80-cell baseline because KV size is now a sweep dim per Q6;
  was 4 in first attempt) — gated by `scaling_ycsb_spec.md §13`.

---

## Already-done from first iter-4A attempt (reusable)

The first attempt was committed at `78fc789` and earlier; not all of
it is wasted. Items that survive into the REPLAN:

- [x] Spec written: `docs/design_goals.md §Protocol A (§I-XIII)`.
- [x] §VI-A primitive reference table.
- [x] §VI-A.bis NT-vs-clflushopt decision rule + decision card.
- [x] §VI-B 22-scenario implementation cheat sheet.
- [x] §X enforcement framework (H1-H4 + P1-P3).
- [x] §XI PR review checklist.
- [x] §XII open questions (O1-O5).
- [x] §XIII Reviewer Attack Process (RAP).
- [x] CLAUDE.md updated with RAP triggers + hard enforcement.
- [x] `src/cxl_sharding.{h,cc}` (Phase 1) — likely usable as is,
  Phase 1 of REPLAN re-validates against `o_i2_owner_routing_uniform`.
- [x] `src/cxl_directory.{h,cc}` (Phase 2) — likely usable as is,
  Phase 2 of REPLAN re-validates against `o_i5_per_slot_no_false_share`.
- [x] `src/cxl_cache_pool.{h,cc}` (Phase 3) — likely usable as is,
  Phase 3 of REPLAN re-validates against `o_i3` and `o_i4`.
- [x] `src/cxl_kv_blockpool` (Phase 4) — likely usable as is.
- [x] `src/cxl_per_host_ring.h` extended for 5 message types
  (Phase 5 partial) — receiver bodies must be EXTENDED to real
  handlers in REPLAN Phase 5a (3 metadata types) + 5b
  (write_forward); the type union and message struct layout survive.
- [x] `src/cxl_forward_ring.h` + ForwardRingMatrix
  (Phase 8 partial) — usable; Phase 5.5 audits its atomics; Phase 8
  REPLAN re-uses ForwardStaging integration from Phase 5b.
- [x] iter-4A first-attempt summary at
  `docs/iters/iter4A_summary_20260430.md` — preserved as
  cautionary precedent; do not delete.

Items to **rewrite** in REPLAN:
- `src/cxl_kv_ops_A_v2.{h,cc}` → renamed to `src/cxl_kv_ops_A.{h,cc}`
  in REPLAN Phase 5a (cleanup commit at start of phase). Logic in
  `execute_write_local` rewritten in REPLAN Phase 6 (real
  invalidate); `search` rewritten in REPLAN Phase 7 (real register).
- All `// TODO Phase 7/8` and `// Phase 7 wires that` comments
  must be gone after REPLAN Phases 6/7.

---

## Decisions taken (2026-05-02)

The 5 open questions raised in the first draft of the REPLAN have
been resolved by user direction; recorded here for later
cross-referencing.

**Q1 — AP3 / AP10 dedicated oracle?** No. AP3 is structurally caught
by `o_i3_cache_map_shared` (a per-worker private cache would make
worker-1 miss worker-0's set_stale → oracle FAILs). AP10 is caught
by `o_i8_no_lfm_in_protocol_a` (LFM acquire counter must be 0). The
oracle table explicitly notes these coverage chains so future audits
don't ask "where's the AP3 test."

**Q2 — `o_i1` / `o_i10` SIGKILL+restart fixture**: implemented
**self-contained** inside `tests/oracle_harness.h` as
`kill_peer(host_id, pgrep_pattern)` + `restart_peer(host_id,
binary_path, args, cookie)`. Does NOT reuse `crash-recover-test/`
(which is RDMA-era client/server architecture and doesn't fit the
unified-node iter-4A model).

**Q3 — `o_ap16` runtime instrumentation**: rejected the brittle "≤ 5
instructions between atomic store and flush_line" check. Replaced
with a **two-layer defense**:
- Layer 1 (macroscopic, runtime, oracle-style): the `o_ap16` oracle
  uses `perf stat -e uncore_imc/cas_count_write/` over a 1M-store
  stress test against a CXL-resident ring; PCIe-write count ∈
  [0.95M, 1.05M] = PASS. Black-box, no compiler-reorder
  false-positives.
- Layer 2 (microscopic, review-time, audit-style): Phase 5.5's
  `iter4A_cxl_atomic_audit.md` lists every CXL-resident atomic site
  for human reviewer sign-off.

**Q4 — Phase 5 split**: predetermined into **5a (metadata-only:
invalidate / register / evict)** + **5b (value-carrying:
write_forward + ForwardStaging)**. No "decide at runtime" judgment
call — that pattern historically defaults to "don't split" once
mid-stream.

**Q5 — oracle count**: 12 → **13**, adding
`o_i9_fast_path_no_cxl_access` to cover §I9's *performance* half
(the existing `o_i9_concurrent_read_write_no_stale` covers only the
race-correctness half). The new oracle uses
`measure_pcie_reads_from_dax(fn)` to verify the read fast path does
not fall through to any CXL atomic.

**Q6 — Variable-KV blockpool integration scope**: B (in iter-4A,
not deferred). Rationale: iter-4A sweep numbers should be directly
usable for paper headlines; deferring leaves a kv-size gap between
"correctness milestone" and "production data point" that iter-5A
would inherit. Concretely:
- Phase 4 ships size-class allocator (256/512/1024 B) + per-host 2
  GB segment + epoch defer-free.
- Phase 6 `execute_write_local()` allocates from blockpool, NT-
  stores value bytes into block, slot stores `(block_ptr,
  size_class)` not inline u64. **Hard break from iter-3A's
  inline-u64 slot.**
- Phase 7 `search()` cross-host miss receives `(block_ptr, size)`
  via OP_CACHE_REGISTER response, fetches value bytes from CXL
  block via `clflushopt + mfence + load`. NO inline payload in any
  message (per §VI-A.bis).
- Phase 8 cross-host write: ForwardStaging sized for max KV
  (1 KB × T=86 × 2 outstanding ≈ 176 KB; 1 MB / host headroom
  unchanged).
- Phase 10 sweep adds KV size dim: 80 cells → 240 cells × 5 reps
  = 1200 SUMMARY lines; wall-clock ~3-4.5 h.
- New 14th oracle `o_kv_blockpool_integrated` flips GREEN at
  Phase 6, catches "protocol path silently uses inline u64".
- `scaling_ycsb_spec.md` §3 requires companion edit at start of
  Phase 10 to declare `KV_SIZES="256 512 1024"`.

---

## Ready to start Phase 0.5

All structural decisions resolved. Phase 0.5 deliverable list:

1. `tests/oracle_harness.h` (~230 LOC; includes kill_peer /
   restart_peer + perf-counter helper).
2. `tests/protocol_a_invariants.cc` (~470 LOC; 14 oracle bodies).
3. `tests/CMakeLists.txt` entry for `protocol_a_invariants` +
   `protocol_a_invariants_tsan`.
4. `scripts/run_invariant_oracles.sh` cross-host driver.
5. `src/perf_counter_helper.{h,cc}` (used by `o_ap16` and
   `o_i9_fast_path_no_cxl_access`).
6. Build on g3/g4; run `bash scripts/run_invariant_oracles.sh`;
   confirm `14 oracles, 0 PASS, 14 FAIL` reported.

Phase 0.5 commit prefix: `[iter4A-blueprint][I1..I12][AP3][AP10][AP16]`
(per H4 hook, multiple I/AP refs in single commit OK).
