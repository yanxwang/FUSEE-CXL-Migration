# iter-14A Phase 2 — Cross-host Write Self-Invalidate RAP (§XIII)

**Date**: 2026-05-19
**Plan**: [task_plan_iter14A.md](task_plan_iter14A.md) Phase 2
**Fix ID**: F1 (iter14A_fix_register.md)
**Compile flag**: `-DFUSEE_XHOST_WRITE_SELF_INVAL=1`
**LOC estimate**: ~30-50

---

## STATE

### Current write path (cross-host, A=writer worker on host A, B=owner)

```
A(worker) → forward_write → WriteRing[A][B] → B(WriteReceiver)
                                                  ↓
                                             execute_write_local on B's bucket
                                                  ↓
                                             scan dir.sharer_bitmap
                                                  ↓
                                             for each sharer h ≠ B:
                                                send_invalidate(h, key)  ★
                                                  ↓
                                                wait ack from h's InvalReceiver  ★
                                                  ↓
                                             commit slot pointer
A ← ack ← B
```

The starred path executes **once per non-self sharer**. In 2-host
system, when A was a previous sharer (i.e. A had cached key K via earlier
forward_read), this means **1 invalidate roundtrip A↔B per cross-host
write** (B sends inval to A, waits for A's InvalReceiver ack).

Measured cost: 1 InvalRing roundtrip ≈ 5-8 µs per cross-host write that
hits the "A is sharer" case.

### Proposed change

```
A(worker)
  1. cache_pool_set_stale(key) + tls_evict(key)       [local ~200 ns]
  2. forward_write → WriteRing[A][B] → B(WriteReceiver)
                                          ↓
                                     execute_write_local with src=A skip
                                          ↓
                                     scan sharer_bitmap & ~(1<<A)        ★ skip src
                                          ↓
                                     in 2-host: nothing to invalidate    ★ saves rt
                                          ↓
                                     commit slot pointer
A ← ack ← B
```

A self-invalidates its local cache BEFORE sending the WriteEntry.
B's WriteReceiver excludes src=A from the invalidate broadcast loop.

**Saved**: 1 InvalRing A↔B roundtrip per applicable cross-host write.

---

## ATTACK VECTORS (6 categories)

### 1. PERFORMANCE

- **Win**: 5-8 µs per cross-host write that previously triggered A
  invalidate. In workload-a (50% write Zipf θ=0.99), ~50% of writes are
  cross-host; of those, maybe 70-90% involve A being a previous sharer
  (because Zipf hot keys are repeatedly read+written). Expected:
  **5-15% workload-a throughput improvement at high T**, larger on
  bench_xhost_write microbench.
- **Risk of slowdown**: worker side adds 2 atomic stores + 1 flush_line
  per cross-host write (cache_pool_set_stale + tls_evict). Cost
  ~150-300 ns. Net: still win unless invalidate roundtrip was already
  cheap.
- **Verifiable by**: P6 bench_xhost_write microbench A/B; production
  workload-a sweep A/B.

### 2. CORRECTNESS — §I9 strict-A linearizability

Four race cases analyzed:

**Case 1 — single-thread sequential write→read on A**: ✓ SAFE
- A.t0: forward_write(K, v_new) self-invalidates K's cache
- A.t1: forward_write returns ACK (B committed new value)
- A.t2: A reads K → cache MISS → forward_read to B → returns v_new

**Case 2 — concurrent thread on A reads K during forward_write in flight**: ✓ SAFE
- t0: worker-1 on A calls forward_write(K, v_new), self-invalidates
- t1: worker-2 on A reads K, cache MISS, forward_read to B
- B's bucket may still have v_old at this moment (B hasn't processed
  worker-1's WriteEntry yet) → worker-2 reads v_old
- Strict-A check: linearization point of worker-1's write = WriteEntry
  enqueue. worker-2's read started BEFORE worker-1's write committed
  → linearizable order [worker-2 read, worker-1 write] → worker-2
  reading v_old is correct.
- Critical sub-invariant: self-inval MUST happen BEFORE WriteEntry
  enqueue (program order on a single thread is enough; no fence
  needed for x86 since both are stores → release ordering by default).

**Case 3 — host-1 reader of K**: ✓ SAFE (unchanged from baseline)
- host-1 had registered as sharer via earlier forward_read
- When A writes, B's invalidate broadcast still includes host-1
  (only A is excluded from broadcast). host-1's cache gets invalidated.

**Case 4 — A was NOT a previous sharer of K**: ✓ SAFE & trivial
- A had no cached entry → set_stale is no-op
- sharer_bitmap doesn't have A bit set → broadcast loop's
  `(bitmap & (1<<A)) == 0` check already skips A → exclusion is no-op
- No change in behavior, no overhead added (almost — the 2 atomic
  stores still execute but are local DRAM, cheap)

**Subtle point — sharer_bitmap state after**:
- Currently after execute_write_local: `sharer_bitmap = (1 << owner)` (only B)
- This is unchanged. Writer A is NOT added back as sharer post-write.
  A must re-register via forward_read on next access. Correct.

**Test plan**:
- Hash-diff 20-cell battery on build-cxl-p2: must PASS
- G6 protocol_a_rw_race_test 100k iters on build-cxl-p2: must PASS
  (This test is specifically designed to catch concurrent R/W races on A.)

### 3. GENERALITY

- **2-host** (g3/g4 testbed): saves ALL invalidate broadcasts when A
  was a sharer (only 1 non-owner sharer = A, which is excluded → 0
  invalidates).
- **≥3-host system**: saves 1 invalidate (to A) per cross-host write
  where A was a sharer. Other sharers (host-2, host-3, ...) still get
  invalidated. Smaller relative win but still a win.
- **Always safe**: in any N-host system, excluding only the writer
  from invalidate is correct per §I9 (writer's cache is already
  invalidated by itself).
- **Backward compat**: build flag default OFF preserves baseline
  behavior. Switching ON via CMake or env requires no protocol
  agreement between hosts (independent flag per host).

### 4. COMPLEXITY

- **Code added**: ~30-50 LOC across 2 files.
- **New API**: `execute_write_local` and `execute_write_local_with_blk`
  gain optional last parameter `int self_inval_src = -1` (default = no
  skip).
- **New compile flag**: `FUSEE_XHOST_WRITE_SELF_INVAL` (0/1, default 0).
- **No new files**, no new threads, no new CXL structures.
- **Build matrix impact**: adds 1 build dir (`build-cxl-p2`) on each
  host alongside existing `build-cxl`. Manageable.

### 5. PRIOR ART

- **MESI protocol writer self-invalidate**: standard pattern. When a
  cache line transitions to M state on core X, all other cores' copies
  are invalidated; core X's copy is the only valid one. The proposed
  change is the same idea at protocol level — writer's local cache is
  authoritatively stale once it issues a write, so it can self-inval
  + signal "exclude me from broadcast".
- **iter-13A Phase 2 W1**: introduced "src bumps local allocator", a
  similar "writer takes more local action to amortize cross-host work".
  This RAP is in the same family.
- **Lock-free / wait-free literature**: "writer issues invalidate to
  followers but skips self" is the standard pattern.

### 6. IMPLEMENTATION FEASIBILITY

- **Hash-diff scaffold**: already exists (iter9A_redo_hash_diff_battery.sh)
- **G6 test**: protocol_a_rw_race_test already builds on this iter's
  HEAD (fixed in P1)
- **Microbench harness**: forward_write already accepts owner host; for
  measurement use existing workload-{a,d,f} cross-host traffic patterns
  via cache=off forcing cross-host. Full bench_xhost_write trace
  generator is P3.A (later) — for P2.3 quick gate, use cache=off
  workload-d 50% write at T=4/32/64 → known to have substantial
  cross-host write traffic.
- **Rollback path**: build flag default OFF (CMake `option(...)`); if
  measurement gate FAILs, flag stays default OFF in CMake, code stays
  in tree but inactive. iter-15A can re-enable or remove.

---

## ABLATION CHECK

Build comparison:
- `build-cxl`: baseline (FUSEE_XHOST_WRITE_SELF_INVAL=0 default)
- `build-cxl-p2`: candidate (FUSEE_XHOST_WRITE_SELF_INVAL=1)

Cell-set comparison (5 reps each):
- **Target**: workload-d cache=off T={4, 32, 64} KV=1024 — these force
  cross-host writes (cache=off means no local cache for reads either,
  but workload-d is 95% read 5% insert; INSERTs are cross-host. Use
  workload-a 50% write as primary target.)
- Revised target: workload-a cache=off T={4, 32, 64} KV=1024 — 50% of
  ops are UPDATE → ~50% trigger cross-host write per Zipf distribution.
- **Guardrail**: workload-c cache=on T=64 KV=1024 (read-heavy, should
  not be affected); workload-b cache=on T=64 KV=256 (mostly local
  cache hits, should not regress)

Threshold (per universal fix policy, medium LOC ~50):
- Target: median +5% improvement, CI lower bound > +1%
- Guardrail: no regression > 2%

---

## PRIOR ART CHECK

Existing relevant code:
- `cxl_kv_ops_A.cc::execute_write_local` Step 4 already loops over
  sharer_bitmap excluding `host_id_`. The proposed change extends the
  exclusion to also skip `src` host.
- iter-13A Phase 2 W1 added `execute_write_local_with_blk` — the same
  exclusion logic is duplicated there and needs identical update.
- `cache_pool_set_stale` and `tls_evict` are both pre-existing APIs
  used elsewhere; no new code surface needed worker-side.

No conflicts with iter-12A Phase 5 stale-cache fix (that fix is in
attach path, not the write path).

---

## VERDICT

**Low risk, high reward**:
- Correctness: 4 race cases analyzed, all SAFE. Hash-diff and G6 test
  will catch any miss.
- Performance: predicted 5-15% workload-a improvement; bench_xhost_write
  microbench will show the saving cleanly.
- Complexity: ~30-50 LOC, build-flag-gated, rollback trivial.
- Compatibility: prior-art-aligned, no protocol break.

**DECISION**: implement; gate ship on measurement (P2.4 decision).

---

## P2 Phase execution checklist

- [ ] 2.1 Implement
  - [ ] Add `FUSEE_XHOST_WRITE_SELF_INVAL` option to CMakeLists.txt
  - [ ] Add `self_inval_src` parameter to execute_write_local + execute_write_local_with_blk
  - [ ] Update invalidate broadcast loop in both functions
  - [ ] forward_write_direct: add cache_pool_set_stale + tls_evict pre-call
  - [ ] write_handler: pass `src` to execute_write_local_*
  - [ ] Compile clean (build-cxl-p2 succeeds on both hosts)
- [ ] 2.2 Correctness
  - [ ] Hash-diff 20-cell battery on build-cxl-p2: 20/20 PASS
  - [ ] G6 protocol_a_rw_race_test 100k iters on build-cxl-p2: PASS
- [ ] 2.3 Measurement
  - [ ] Target cells: workload-a T={4,32,64} cache=off KV=1024 × 5 reps × 2 builds
  - [ ] Guardrail: workload-c T=64 cache=on KV=1024 × 5 reps × 2 builds
  - [ ] Bootstrap 95% CI on median delta
- [ ] 2.4 Decision
  - [ ] PASS → flag default ON in CMake; F1 register "win"
  - [ ] FAIL → flag default OFF; F1 register "rollback with data"
- [ ] 2.5 Stage spec update if PASS (W4/W6 broadcast stages reduced)
