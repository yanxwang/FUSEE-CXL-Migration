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

12 phases. Each phase has:

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

**Goal**: Land an `oracle_harness` test framework + 12 fail-first
invariant oracles. After this phase the `protocol_a_invariants`
binary exists and reports 12/12 RED. Every subsequent phase's
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

  // Failure macro that records location + dumps SUMMARY before abort.
  #define ORACLE_REQUIRE(cond) ...
  ```
  Backed by a 2 MiB region at the end of the CXL mapping (reuses
  scaling_ycsb's shared-stats convention; orchestrator-supplied
  `FUSEE_RUN_COOKIE`).

- NEW `tests/protocol_a_invariants.cc` (~400 LOC, 12 oracle bodies):

  | Oracle | Spec ref | host_count | tsan | What it tests |
  |--------|----------|-----------|------|---------------|
  | `o_i1_recover_from_cxl` | §I1 | 2 | no | host A inserts 1k keys, hard-kills its process; restart, host B's reader fetches all 1k from CXL byte-equal |
  | `o_i2_owner_routing_uniform` | §I2 | 1 | no | 100k synthetic keys → owner_host distribution within 5 % of uniform; same key hashes to same owner across calls |
  | `o_i2_xhost_writes_use_forward` | §I2/I11 | 2 | no | 1000 cross-host writes; counter `forward_to_owner_calls == 1000`, `execute_write_local_xhost == 0` |
  | `o_i3_cache_map_shared` | §I3 | 1 | no | 4 fork workers; worker 0 set_stale; worker 1 lookup sees `stale==1` without IPC |
  | `o_i4_lazy_not_physical` | §I4 | 1 | no | invalidate K then lookup K → returns entry pointer (not NULL) with `stale==1` |
  | `o_i5_per_slot_no_false_share` | §I5/I7 | 1 | no | 16 same-host workers write 1M ops to distinct slots in same bucket; per-thread tput within 30 % of single-thread tput (no contention) |
  | `o_i8_no_lfm_in_protocol_a` | §I8 | 2 | no | run 5k ops mixed; LFM `acquire_count == 0`; spinlock `acquire_count > 0` |
  | **`o_i9_concurrent_read_write_no_stale`** | **§I9** | **2** | **yes** | **host A reads K (populates cache); host B updates K; host A reads K again — MUST see new value. Run 1000 iterations. This oracle is the one iter-4A's first attempt missed.** |
  | `o_i10_writer_durable_after_return` | §I10 | 2 | no | host A writes K, returns; SIGKILL host A immediately; host B reads K from CXL — must see new value (commit point includes CXL durable + sharer ACK before return) |
  | `o_i11_xhost_forward_observable` | §I11 | 2 | no | forwarder publishes ForwardRing tail via fetch_add; owner host sees new tail within 100 µs (verifies CXL atomic flush plumbing end-to-end) |
  | `o_i12_no_oplog_calls` | §I12 | 1 | no | run 1k ops; `oplog_begin_count == 0`, `oplog_commit_count == 0` |
  | `o_ap16_cxl_atomic_flush_paired` | §VII AP16 (new) | 1 | no | scans symbol table at link time + runtime instrumentation: every CXL-resident atomic store/RMW emits a `flush_line` event within 5 instructions in the same source location (introspection via debug build flag) |

- NEW `tests/CMakeLists.txt` entry:
  `add_executable(protocol_a_invariants ...)` plus a TSan variant
  `protocol_a_invariants_tsan` for oracles with `needs_tsan=true`.

- NEW `scripts/run_invariant_oracles.sh`: cross-host driver that
  ssh-launches the binary on g3+g4 with matching `FUSEE_RUN_COOKIE`
  and aggregates per-oracle PASS/FAIL.

**Validation experiment**:

1. Build `protocol_a_invariants` and `protocol_a_invariants_tsan`.
2. Run on g3+g4: `bash scripts/run_invariant_oracles.sh`.
3. **Expected output**: `12 oracles, 0 PASS, 12 FAIL`. Every
   oracle fails because no protocol code exists yet. This is the
   GREEN condition for Phase 0.5: ALL RED.

**Success criterion**: `protocol_a_invariants` binary builds; oracle
runner driver works (cross-host barrier, fork helper, timeout, TSan
build); `12/12 FAIL` reported with each failure citing a clean spec
reference. NO oracle accidentally PASSing (a green oracle now means
the test is misimplemented — abort and fix the oracle).

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

## Phase 4 — KV blockpool: per-host partition + CoW write API

**Goal**: per-owner-host blockpool segments on CXL; host-local DRAM
free-list; `blockpool_alloc/free` API exposed but not yet wired
into protocol.

**Spec coverage**: I6, §VI synchronization.

**Code changes**: MODIFIED `src/cxl_kv_blockpool.{h,cc}` — partition
into `H` segments + per-segment host-local spinlock.

**Oracles affected**: none flip yet (no protocol path).

**Bottleneck check**: alloc ≤ 50 ns uncontended; per-host segment
isolation strict (cross-host alloc returns 0).

---

## Phase 5 — Message channel **fully wired** (5 message types + receivers)

**Goal**: NOT a "type definition + counter stub" phase like the
first attempt. Phase 5 lands all 5 message types AND their
receiver-side handler logic. After this phase, Phase 6/7/8 only
attach the sender side at the KV API entry points; receivers don't
get rewritten later.

**Spec coverage**: I11, §III ForwardStaging, §XII O3 no inline
payload, §VI sync rules.

**Code changes**:

- MODIFIED `src/cxl_per_host_ring.h`: rename `PerHostInvalEntry` →
  `PerHostMessage`; entry size stays 64 B (preserve cacheline
  discipline); union-by-`op_type` with fields for the 5 types.

- NEW `src/cxl_forward_staging.{h,cc}`: per-host ForwardStaging
  buffer on CXL (1 MB / host); host-local DRAM free-list metadata;
  alloc/free API.

- MODIFIED `src/cxl_kv_ops_A.{h,cc}` (the protocol-A class —
  refactored from the old `_v2` filename via a separate cleanup
  commit at the start of this phase) — add `receiver_loop_full()`:
  - **OP_INVALIDATE handler**: take owner-side directory lock for
    target slot; `cache_pool_set_stale(key)`; clear `sharer_bitmap`
    bit for sender host; release; reply OP_RESPONSE.
  - **OP_CACHE_REGISTER handler**: take directory lock; set
    `sharer_bitmap` bit for sender; bump `version`; copy current
    value pointer; release; reply OP_RESPONSE with value pointer.
  - **OP_CACHE_EVICT handler**: take directory lock; clear
    `sharer_bitmap` bit; if bitmap == 0 set state INVALID; release;
    reply OP_RESPONSE.
  - **OP_WRITE_FORWARD handler**: fetch value from forwarder's
    ForwardStaging via `clflushopt + mfence + load`; call shared
    `execute_write_local()` (introduced as a static helper, used by
    both owner-self and forward paths); reply OP_RESPONSE.
  - **OP_RESPONSE handler**: sender side; mark request slot ready.

- NEW `tests/n11n_full_dispatch_test.cc`: for each of 5 op types,
  inject 1k from g3 → g4, verify (a) g4 receiver counter == 1k,
  (b) g4 directory state changed correctly per op type,
  (c) g3 receives matching OP_RESPONSE within timeout.

- NEW `tests/forward_staging_roundtrip_test.cc`: 100k roundtrips at
  value sizes {256, 512, 1024} B; zero byte mismatch; zero staging
  slot leak; p99 ≤ 6 µs.

**Oracles affected**:
- All oracles that need cross-host messaging (`o_i9`, `o_i10`,
  `o_i11`) become *runnable* but most still fail at the KV-API
  level (no API path yet uses them).
- `o_i11_xhost_forward_observable`: RED → GREEN (the message-
  channel-level test passes; KV-API-level tests still fail until
  Phase 8).

**Bottleneck check**: message round-trip p50 ≤ 3 µs (sender enqueue
+ flush 200 ns + receiver consume 1.5 µs + response 1.3 µs).

**Why this phase changed from the first attempt**: original Phase 5
defined types but receivers were counter stubs. Phases 6/7/8 each
wrote `// TODO future phase` for receiver behavior. By landing the
receivers fully here, no later phase can introduce a TODO chain.

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

**Code changes**:
- NEW `docs/iters/iter4A_cxl_atomic_audit.md`: table of every
  `std::atomic` / `fetch_add` / `compare_exchange*` / `.store(` /
  `.load(` in `src/cxl_*.{h,cc}`, classified:
  - **DRAM-resident** (cache pool, directory): no flush needed.
  - **CXL-resident** (rings, ForwardStaging metadata, anything in
    `/dev/dax0.0` region): must have flush_line + sfence pair.
  - Each row: `file:line | resident | producer flush OK | consumer
    flush OK | fix commit`.
- ADD entry AP16 to `src/cxl_kv_ops_A.h` H1 trip wire: optional
  debug-build instrumentation that crashes if a CXL-resident
  atomic is written without a `flush_line` within N instructions
  (pragmatic heuristic — see open question Q4).

**Oracles affected**:
- `o_ap16_cxl_atomic_flush_paired`: RED → GREEN (only after audit
  table is 100 % OK and instrumentation is enabled in debug build).

**Bottleneck check**: audit instrumentation in debug build ≤ 5 %
runtime overhead (release build no overhead).

---

## Phase 6 — Writer owner-self full integration

**Goal**: wire the owner-self write path on the **complete** message
channel from Phase 5: directory spinlock → blockpool alloc + CoW
write to CXL → **send OP_INVALIDATE to every sharer** (real, not
stub) → **wait OP_RESPONSE** → atomic CAS slot.pointer → update
directory state → update local cache.

**Spec coverage**: I9 (write side), I10, I6, I8.

**Code changes**:
- NEW `src/cxl_kv_ops_A.cc::execute_write_local()` (replacing the
  old _v2 implementation): full sequence as above. The Phase 5
  receivers already handle invalidate; this phase only ADDS the
  sender call site.
- MODIFIED `tests/cxl_ycsb_runner.cc`: select Protocol A as
  `CONSENSUS_OPT_A`; existing build flag, no rename.

**Oracles affected**:
- `o_i10_writer_durable_after_return`: RED → GREEN (only if
  invalidate ACK wait is genuinely synchronous; if asynchronous
  optimization is added, oracle catches it).
- `o_i9_concurrent_read_write_no_stale`: stays RED until Phase 7
  wires register-then-fill on read side. **THIS IS THE KEY
  CONSTRAINT THAT WAS MISSING IN THE FIRST ATTEMPT.** Phase 6
  cannot be marked complete with `o_i9` GREEN — it can only show
  the writer-side half.

- Hash-diff battery (existing-style oracle, kept as defensive
  cross-check):
  - 5 reps × T={2,4,8,16} × 100K UPDATEs using **workload-split-
    by-owner** (Option C); host 0 dispatches owner=0 ops, host 1
    dispatches owner=1 ops; build flag `kPhase6FilterUnownedOps =
    true`, removed in Phase 8.

**Bottleneck check**: per-op latency at T=4 owner-self uncontended
≤ 5 µs.

---

## Phase 7 — Reader register-then-fill (slow path) — **THE FIX**

**Goal**: wire `search()` slow path: on cross-host miss, send
OP_CACHE_REGISTER to owner; receive value via OP_RESPONSE; THEN
populate local cache. Phase 5 receiver already updates owner
directory's `sharer_bitmap`. Phase 7 is purely the sender side.

**Spec coverage**: I9 (read side, register-then-fill), AP15.

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A.cc::search()`: cross-host miss path
  enqueues OP_CACHE_REGISTER, spins on response, fills cache only
  AFTER response carries value bytes.
- AP15 trip wire: assert that `cache_pool_insert(key, value)` is
  preceded by `register_acked(key)` flag set; abort otherwise.
  Implemented as a debug-build `cache_pool_insert_after_register()`
  wrapper.

**Oracles affected**:
- `o_i9_concurrent_read_write_no_stale`: RED → GREEN. **This is
  the oracle iter-4A's first attempt skipped.** It must be GREEN
  at this phase or Phase 7 is not complete.

**Bottleneck check**: read fast path ≤ 60 ns p50; cross-host miss
(register round-trip) ≤ 4 µs p99.

---

## Phase 8 — Cross-host write forward + cache evict

**Goal**: enqueue OP_WRITE_FORWARD via ForwardStaging when
`owner != self`; spin on OP_RESPONSE. Wire LRU pressure → enqueue
OP_CACHE_EVICT to owner.

**Spec coverage**: I11, AP14.

**Code changes**:
- MODIFIED `src/cxl_kv_ops_A.cc::{update,insert,remove}`: check
  owner; if remote, allocate ForwardStaging slot, NT-store + sfence
  value bytes, enqueue OP_WRITE_FORWARD, spin response, free
  staging.
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

1. Full sweep:
   - 1 protocol × 5 workloads × 8 T × 2 cache × 5 reps =
     **400 SUMMARY.log lines** (= 80 unique cells × 5 reps).
   - Output to `docs/g34_scaling_ycsb_<timestamp>/`.
   - All 5 G1-G5 gates pass each cell.

2. Required artifacts in the timestamped directory:
   - `SUMMARY.log` (with `rep=<r>` field).
   - `plot_commit.txt`.
   - 5 × `A_thpt_workload*.png` (5-rep median) +
     5 × `*_band.png` (min/max band).
   - up to 10 × `A_lat_workload*_{read,write}.png`.
   - `cache_off/` with the same plot set.
   - `extra/A_target_workload*.png` (5; gap-to-20-Mops/s line).
   - `extra/A_scaling_efficiency.png`.
   - `summary_table.md`.
   - **`gap_to_target.md`** (auto-generated from SUMMARY.log).
   - new row in `docs/scaling_ycsb_runs_index.md`.

3. iter-4A summary doc `docs/iters/iter4A_summary_<ts>.md`:
   - 5-rep median headline numbers per workload at peak T.
   - Stage attribution via Phase 6 latency decomp.
   - Spec drift audit (P2): each I/AP traced to a code location
     with grep evidence; 12/12 oracle GREEN evidence pasted.
   - RAP retrospective: which Phase 1-9 design choices were
     verified by sweep data, which were not.
   - Cite the sweep `<timestamp>` per scaling spec §13.

4. iter-5A candidate list (≥ 3 candidates, each with full RAP per
   §XIII).

**Success criterion (HARD GATE)**:

- 400/400 SUMMARY.log lines ≠ FAIL.
- 12/12 oracles GREEN.
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

| Oracle | P0.5 | P1 | P2 | P3 | P4 | P5 | P5.5 | P6 | P7 | P8 | P9 | P10 |
|--------|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `o_i1_recover_from_cxl` | R | R | R | R | R | R | R | R | R | **G** | G | G |
| `o_i2_owner_routing_uniform` | R | **G** | G | G | G | G | G | G | G | G | G | G |
| `o_i2_xhost_writes_use_forward` | R | R | R | R | R | R | R | R | R | **G** | G | G |
| `o_i3_cache_map_shared` | R | R | R | **G** | G | G | G | G | G | G | G | G |
| `o_i4_lazy_not_physical` | R | R | R | **G** | G | G | G | G | G | G | G | G |
| `o_i5_per_slot_no_false_share` | R | R | **G** | G | G | G | G | G | G | G | G | G |
| `o_i8_no_lfm_in_protocol_a` | R | R | R | R | R | R | R | **G** | G | G | G | G |
| **`o_i9_concurrent_read_write_no_stale`** | R | R | R | R | R | R | R | R | **G** | G | G | G |
| `o_i10_writer_durable_after_return` | R | R | R | R | R | R | R | **G** | G | G | G | G |
| `o_i11_xhost_forward_observable` | R | R | R | R | R | **G** | G | G | G | G | G | G |
| `o_i12_no_oplog_calls` | R | R | R | R | R | R | R | **G** | G | G | G | G |
| `o_ap16_cxl_atomic_flush_paired` | R | R | R | R | R | R | **G** | G | G | G | G | G |

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
- Variable-KV blockpool full integration into A path (kv 256/512/
  1024). Phase 4 wires the API; full integration deferred so
  iter-4A can reach a stable correctness milestone first.
- K-shard responder threads (forward path scaling). iter-4A's
  single responder is the scale ceiling; iter-5A first task.
- BucketLockTable removal from Protocol A's region (saves ~2.5 GB).
- Read-path cache=on collapse for read-heavy workloads (iter-3A
  open issue carried forward).

---

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Oracle harness cross-host coordination flaky on g3/g4 | Med | Phase 0.5 explicit barrier latency budget (5 ms p99); if exceeded, fall back to file-based barrier on local NFS rather than CXL shared region |
| `o_i9` race oracle hits TSan false positives on cross-host atomics | Med | TSan only used for same-host portions of the test; cross-host correctness verified by value-equality, not TSan |
| Phase 5 receiver scope expansion blows up the phase to too-many-LOC | Med | If receiver code > ~800 LOC, split Phase 5 into 5a (invalidate + register + evict) and 5b (write-forward + response). Decision deferred to start of Phase 5 |
| Phase 5.5 atomic audit finds atomics in iter-3A code paths still in tree | Med-High | Audit table records "needs fix" rows; fix in same phase before advancing. Do NOT skip with "iter-3A code is legacy" — protocol C still uses some of those code paths |
| Phase 10 sweep takes longer than budget (60-90 min) | Low | Per-cell timeout 600 s + cell-skip with user approval recorded in summary; all 80 cells must report some result (timeout or value), not "skipped" |
| Phase 10 reveals 12/12 oracles GREEN but throughput still very low (forward path bottleneck) | High | This is *expected* — iter-4A's goal is correctness, not the 20 Mops/s target. iter-5A K-shard responder is the throughput follow-up. Phase 10 must still report the gap honestly in `gap_to_target.md` |

---

## Quick stats

- **12 phases** (was 10): adds Phase 0.5 (oracle blueprint) and
  Phase 5.5 (atomic audit).
- **~5500 LOC delta** (was ~4200): adds ~600 LOC oracle harness +
  oracle bodies, ~700 LOC Phase 5 expansion (full receivers).
- **12 invariant oracles** (was 0 invariant-specific tests, only
  hash-diff). Hash-diff retained as defensive cross-check.
- **400-cell sweep** (was 4 in first attempt) — gated by §13.

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
  handlers in REPLAN Phase 5; the type union and message struct
  layout survive.
- [x] `src/cxl_forward_ring.h` + ForwardRingMatrix
  (Phase 8 partial) — usable; Phase 5.5 audits its atomics; Phase 8
  REPLAN re-uses ForwardStaging integration.
- [x] iter-4A first-attempt summary at
  `docs/iters/iter4A_summary_20260430.md` — preserved as
  cautionary precedent; do not delete.

Items to **rewrite** in REPLAN:
- `src/cxl_kv_ops_A_v2.{h,cc}` → renamed to `src/cxl_kv_ops_A.{h,cc}`
  in REPLAN Phase 5 (cleanup commit). Logic in `execute_write_local`
  rewritten in REPLAN Phase 6 (real invalidate); `search` rewritten
  in REPLAN Phase 7 (real register).
- All `// TODO Phase 7/8` and `// Phase 7 wires that` comments
  must be gone after REPLAN Phases 6/7.

---

## Pending user review before Phase 0.5 starts

1. **Oracle list complete?** 12 oracles cover I1-I12 + AP16. Any
   invariant or AP I missed that needs a dedicated oracle?
   Specifically:
   - AP1-AP12 are mostly anti-pattern *design* warnings rather than
     runtime-detectable invariants. Should AP3 (no per-worker
     private cache) and AP10 (no LFM) get their own oracles?
     Currently AP10 is folded into `o_i8_no_lfm_in_protocol_a`.

2. **`o_i1_recover_from_cxl`**: requires kill-then-restart fixture.
   Does the existing `crash-recover-test/` infrastructure support
   that, or do I need to build the kill helper from scratch in
   Phase 0.5?

3. **`o_i10_writer_durable_after_return`**: needs SIGKILL right
   after return + fork-restart-and-read sequence. Same question as
   above.

4. **`o_ap16` runtime instrumentation**: is the "5 instructions
   between atomic store and flush_line" check too brittle (compiler
   may reorder)? Alternative: count `flush_line` calls in unit time
   and cross-check against `atomic_store` calls in CXL region —
   looser but no false positives.

5. **Phase 5 split**: should I commit to 5a/5b split up-front, or
   keep the conditional split per the risk register?

After your confirmation I will start Phase 0.5.
