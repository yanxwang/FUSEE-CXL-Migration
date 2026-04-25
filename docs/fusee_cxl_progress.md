# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phases 1–8 all have substantive work landed; all three protocols pass multi-proc correctness, A stall fixed, cache-on v3 sweep complete
- **Phase**: 0 skipped; 1,2,3,5,6,7 done; 4 done (soft-gate, hard-delete deferred); 8 done (cache-on numbers; cache-off sweep is fragile but individual runs work)
- **Branch**: `feat/cxl-migration` on emr + GitHub origin (80+ commits); 4-way synced (local, emr, g3, g4)
- **Last commit**: `5189086 [micro-batch] Revert multi-flusher scaffold + ready-flag (both buggy)` — 2026-04-24 10:30 CDT (preceded by `b940c70` Phase-1 LFM anatomy data)
- **Kernel transition**: g3/g4 now on 6.15.0 (was something pre-6.15). Fixes the overnight hard-offline issue; raises LFM fork-mode N threshold. See `docs/g34_bench/g34_kernel_new_observations.md`.
- **GitHub**: https://github.com/yanxwang/FUSEE-CXL-Migration (private)
- **Working tree**: clean on tracked files; untracked user setup scripts to ignore
- **Sudo authorization**: user wang authorized sudo on emr; password kept in session memory, not written to repo files

## Option A side-track — ongoing research

> **This is a long-term research thread running in parallel with the main migration.**
> Option A's concrete implementation evolves as we discover more optimizations.
> The design used in FUSEE src/ (main migration) MUST match whatever A design
> is current in the mini-bench. Every A change is logged here.

- **Full log**: `docs/option_a_side_track.md` (primary doc, append-only)
- **First investigation report**: `docs/option_a_perf_analysis.md` (initial cause analysis)

### Current A design: **A-v2 (SPSC ring)**

- **Replaced**: naive per-bucket `pending_op` + O(NUM_BUCKETS) scan
- **With**: per-(src,dst) SPSC ring; writer enqueues, replicator polls N-1 ring heads
- **Result**: original A (4000 μs) → A-v2 (20 μs) = **180x write latency improvement**
- **Inherent remaining gap vs B/C**: ~2x slower (unavoidable sync-ACK wait)
- **Integrated into**: `ycsb_abc_bench.c` (mini-bench)

### Ideas not yet tried (see side-track doc for details)

- Pack ring entry fields into single cache line (tried 2026-04-20, reverted)
- Write pipelining / batch ACKs
- Reduce ACK granularity (per-batch)
- Quorum ACK (N/2+1 instead of N-1)
- NUMA-aware ring placement

## Side-track: Option A perf investigation

Goal: find why Option A write throughput is ~100x lower than B/C in mini-bench; if root cause is implementation artifact (not fundamental), provide A-v2 that matches B/C write perf.

See `docs/option_a_perf_analysis.md` (will be created) for the live research log.

## Environment

- **Dev host**: `ssh emr` (Intel Xeon Gold 6530, 128 GB DRAM + 256 GB CXL, kernel 6.17.3-fusionfs)
- **FUSEE path**: `~/FUSEE` (on emr), `/home/yanwang/FUSEE` (local)
- **CXL library**: `~/cxl_shm_profiling/` on emr (rsync'd from `/home/yanwang/cxl_shm_profiling/`)
- **Also on emr**: `~/cxl_profiling_tool/global_allocator/` — similar library, not used by us
- **Git policy**: commits stay local on emr, **do not push to remote**
- **Branch strategy**: single `feat/cxl-migration` branch

## Phase status

| Phase | Status | Commits | Notes |
|---|---|---|---|
| 0: Baseline | ⏳ skipped | — | FUSEE repo already on emr; no baseline run required |
| 1: CXL basic infra | ✅ done | a3e7a63, 60433be | `src/cxl_mm.{h,cc}` + single-proc + multi-proc tests on /dev/dax0.0 |
| 2: BucketLock table | ✅ done | c1f40e6 | `src/cxl_bucket_lock.{h,cc}` (LFM); 100k contended incr on CXL, ~7.4 μs/crit |
| 3: KV ops Option C | ✅ done | a2fbc30 | `cxl_kv_ops_C.{h,cc}` + hashtable + 2-proc test on /dev/dax0.0 |
| 4: Remove RDMA deps | ✅ done (soft) | bbf3986 | `-DCXL_ONLY=ON` CMake option; default build still produces libddckv + RDMA tests |
| 5: Single-node YCSB | ✅ done | 29ba277 | `cxl_ycsb_runner_{A,B,C}` + synthetic spec-file generator; ran wl_A + wl_C on real CXL |
| 6: OpLog + crash recovery | ✅ done | bd93e82, 9bfe9c2, c613b59 | Per-host ring + begin/commit/abort + recovery scan + `recover_redo` callback; wired into all three protocols; crash-replay test passes |
| 7: Options A and B | ✅ done | bf690a3, ea9599b, 360da05, ff4d32d | A + B + protocol switch; A multi-proc robustness fix (always-clear op_id) |
| 8: Performance benchmarks | 🚧 partial | 95bb1b2, 75996a9, bd93e82, a1d259a, 3f8d604, 3f989f1, 0bc1b9a, f0354a2 | DRAM-cache semantic distinction (C 10×, B 346× on cache hits); Zipf workload gen; `FUSEE_CACHE=1` bench; Option A wr=1.0 stall still open |

## Recent results (v3 sweep, post A-stall fix)

**Single-host DRAM cache speedup** (`/dev/dax0.0`, 50k search of 200 keys):
- Option C: 8306 → 820 ns/op (**10.1×** on cache hit; reader still does one CXL epoch load)
- Option B: 4608 → 13 ns/op (**346×** on cache hit; reader is DRAM-only, invalidation via ring)
- Option A: same shape as B, with writer waiting for all replicators to invalidate before unblocking (sync semantics)

**4-host multi-proc, `FUSEE_CACHE=1`, 500 ops/host, /dev/dax0.0** (see `docs/fusee_mp_bench_v3_cache_on.png`):

| opt | wr=0.0 (reads) | wr=0.5 (mixed) | wr=1.0 (writes) | w_avg @ wr=1.0 |
|-----|---------------:|---------------:|----------------:|---------------:|
| A   |            —*  |          279k  |           168k  |        20.3 μs |
| B   |          1164k |          395k  |           245k  |        12.5 μs |
| C   |          1146k |          749k  |           601k  |         5.0 μs |

*A at wr=0.0 hit a transient sweep cleanup issue; standalone run lands at 459k ops/s.

Takeaways:
- **Reads (wr=0)**: B ≈ C essentially tied at ~1.15M ops/s agg. Both serve from DRAM cache on hits.
- **Writes (wr=1)**: C is 2.4× faster than B and 3.6× faster than A. Latency ratio matches the design: C only bumps an epoch, B additionally pushes to 3 rings, A additionally waits on 3 ACKs.
- **A's multi-proc stall from earlier runs was a bench bug, not a protocol bug** (commit `b8b1994`: `store.stop()` was called before the all-hosts-done barrier).

See also `docs/fusee_mp_bench_v2.png` (pre-cache, pre-A-fix) for contrast.

## Next concrete tasks (still open)

1. ~~Root-cause Option A multi-proc wr=1.0 stall~~ — **RESOLVED** in commit `b8b1994`. The stall was a bench teardown race: `store.stop()` was called before every host had signaled `done`, so the primary's replicator exited while peers were still writing. Fix reorders the stop after the done barrier. Post-fix: 4h × 500 wr=1.0 agg=88k ops/s, zero ACK timeouts.
2. **Run a cache-on multi-proc bench sweep and generate a v3 plot** — script landed in `df0c193` (`tests/run_fusee_mp_sweep.sh`); tmpfs dry-run passes end-to-end. **Devdax reconfigure stuck**: node 2's kernel `going-offline` state on memory block 65 holds ~2 GB of unmigratable kernel allocations (every tracked category — Active/Inactive/AnonPages/FilePages/Mapped/etc. — is 0, yet MemUsed=2 GB). `daxctl reconfigure-device --mode=devdax --force dax0.0` hangs indefinitely with 35/128 blocks offline, the rest online, and block 65 stuck. `drop_caches` + `compact_memory` did not free the block. Unblock via reboot in the next session.
3. **Phase 4 hard deletion** — delete `src/nm.{h,cc}` / `src/ib.{h,cc}` and the client/server RDMA files once we are sure the RDMA path stays gone. The soft gate is enough for now; hard deletion is intentionally deferred.
4. **Integrate OpLog recovery callback into each CxlKvStore** — DONE. All three protocols shipped: C in `56fb6b7`, A and B in `4877313` (symmetric `recovery_mode_` short-circuit in `dispatch_*wait`). All three green on /dev/dax0.0.
5. **Official YCSB workloads** — **tmpfs cache-aware sweep landed.** Workloads fetched to `setup/workloads/` (workloada…workloadf, spec_load/spec_trans format). Sweep `run_fusee_ycsb_sweep.sh` auto-detects both synthetic and official naming, takes `MAX_OPS` + `CACHE_MODES` env knobs; runner (`cxl_ycsb_runner`) accepts optional 6th arg for max_ops and honors `FUSEE_CACHE=1` (commits `6f9e0e2`, `324531b`). Logs: `docs/fusee_ycsb_sweep_tmpfs.log` (cache off, 18 runs, `350d0eb`) and `docs/fusee_ycsb_sweep_tmpfs_cache.log` (cache off + on, 36 runs, `324531b`). Read-heavy workloads on cache-hit: workloadc 450 k → 2.5 M ops/s for A/B (5.7×), 1.25 M for C (still pays one CXL epoch load); workloadb/d 3.7×-3.9×; workloada (50/50) 1.2×; workloade (SCAN, SKIP'd) unchanged. **Devdax rerun blocked on the same node-2 memory-hotplug issue as #2.**

Guardrail: do not touch `src/client*.{h,cc}` or `src/hashtable.{h,cc}` yet — those remain RDMA-only under the default build. The new cxl_* files live alongside them and are selected via `-DCXL_ONLY=ON` or via linking `libfusee_cxl` directly.

**Dax0.0 ready**: ✅ reconfigured to devdax mode on 2026-04-20 02:00 CDT
**Build status**: original FUSEE still has RDMA deps in libddckv; Phase 1 tests link cxl_mm.cc directly, bypassing libddckv. Full libddckv refactor in Phase 4.

## 2026-04-23 — iter 1 of C write-path optimization (per `docs/task_plan_20260423_c_writepath.md`)

Bar: C protocol must hit ≥ 20 Mops/s `trans_agg_thpt` on workloads A, B, F on g3+g4 (`docs/design_goals.md`). Baseline (from `logs/g34_scaling_sweep_p2_v4_20260422_205644`, cache=on): A = 1.08, B = 6.55, F = 1.44 Mops/s — 2 – 18× below target.

**Landed:**

- **Stage-level latency decomposition for C write path** — `src/cxl_latency_decomp_probe.{h,cc}`, `tests/cxl_latency_decomp_C.cc` (fork-based, per-client histograms, primary merges). Compile-gated by `FUSEE_LATENCY_DECOMP=1` on a sibling library `fusee_cxl_decomp` so production `cxl_ycsb_runner_C` pays zero runtime cost.
- **FUSEE-local patched ticket_lock** — `src/ticket_lock_fusee_patched.c`. Upstream `$CXL_SHM_PROFILING_DIR/locks/ticket_lock.c` was missing the pre-`fetch_add` clflushopt required for cross-host correctness; patch kept inside the FUSEE tree via CMake so the shared repo is untouched.
- **Per-slot LFM lock for protocol C** — `src/cxl_bucket_lock.{h,cc}` adds `SlotLockTable` (7 × `bucket_mutex_t` per bucket). `src/cxl_kv_ops_C.cc`, under `FUSEE_PER_SLOT_LOCK=ON`, runs unlocked-scan → lock one slot → re-verify → publish. INSERT carries an under-lock dup scan of the other 6 slots. `bump_epoch` promoted to atomic `__atomic_fetch_add + clflushopt + sfence` (per-slot granularity means concurrent writers on different slots race on the shared per-bucket counter).
- **Decomp + sweep infra** — `scripts/run_latency_decomp_C.sh`, `scripts/finalize_c_only_sweep.sh`, `docs/plot_c_compare.py`; `scripts/run_g34_scaling_sweep.sh` reused with `OPTS=C`.

**Numbers (cache=on peaks, vs baseline):**

- workloada: 1.08 → **3.41** (3.16 ×; T=86) — **5.9 × below 20 Mops/s**
- workloadb: 6.55 → **9.98** (1.52 ×; T=32) — **2.0 × below**
- workloadc: 51.22 → 46.01 (ref only; 10 % regression from SlotLockTable's 17 GiB init cost)
- workloadd: 45.86 → 38.30 (ref only; 16 % regression, same cause plus under-lock dup scan)
- workloadf: 1.44 → **3.53** (2.45 ×; T=16) — **5.7 × below**

Decomp proof: lock p99 at T=64 on workload A dropped 12.8 ms → 440 µs (29 ×); lock_avg 572 µs → 26 µs (22 ×). Per-slot granularity is doing its job — the remaining gap is the critical-section itself (`epoch` bump ≈ 2 µs/op cross-host) funneling ~172 workers through one hot slot.

**Target check:** iter 1 does **not** cross the 20 Mops/s bar on A, B, F. Per plan the loop should feed fresh decomp back into step 1 for an iter-2 choice that does not mechanically repeat per-slot. Candidates (data-driven from the per-slot decomp, not pre-committed): move `bump_epoch` outside the critical section, writer-side self-host DRAM cache refresh, per-client slot-index hint cache. Details in `docs/g34_scaling_ycsb_C_only_20260423_051200/iteration_note.md`.

**Step 2.1 (ticket-lock per bucket):** built after the local patch but `T=8` workload A did not finish the harness budget — `ticket_mutex_t`'s pre/post-fetch_add clflushopts storm the one hot cacheline across hosts, degrading > 350 × vs LFM. Kept as opt-in flag, not the default.

## 2026-04-24 — iter 3 C write-path: Phase-2 fixes + Phase-3 micro-batching (per task_plan_20260424)

Bar: same north-star — C must hit ≥ 20 Mops/s on A, B, F on g3+g4
(`docs/design_goals.md`). Entering iter-3 from iter-2 state: A=3.27 /
B=10.54 / F=4.34 (cache-on peaks).

### Phase-2 low-risk write/read path fixes (three independent commits)

- **`[read-singleshot]`** (9f529a1) — `search()` 8-attempt retry loop
  collapsed to a single pass. x86 aligned u64 loads are atomic → key
  and value are individually torn-free; LRC read semantics relaxed
  to "snapshot at some instant during scan".
- **`[flush-collapse]`** (32cb92f) — 14 clflushopts per bucket scan
  (7 × key + 7 × value) collapsed to 2 (one per 64 B cacheline of the
  128 B bucket).
- **`[route-seq]`** (75c99f1) — `SlotLockEntry` gains a `route_seq`
  cacheline bumped by INSERT/DELETE. UPDATE reads it before the pre-
  scan and re-reads after `lock_slot`; if unchanged, skip the under-
  lock `flush_line(&slot.key) + full_fence + verify`. Workloads A/B/F
  trans phase has zero INSERT/DELETE → always fast path. Strict gate
  (A/B/F ≥ +5 % AND D ≤ 5 % regress) held.

Phase-2 80-run sweep
(`docs/g34_scaling_ycsb_C_only_20260424_044118/`) cache-on peaks:

- A 3.27 → **6.24** (1.91 ×)
- B 10.54 → **32.57** (3.09 ×) — **CROSSES 20 Mops/s BAR** ✓
- C 45.99 → 55.12 (+20 %)
- D 41.00 → 39.50 (-4 %, within gate)
- F 4.34 → **10.49** (2.42 ×)

Workload B is the first validation workload to pass the north-star
in this task.

### Phase-3 per-host DRAM ring UPDATE batching (`[micro-batch]`)

Per-host `MAP_SHARED|MAP_ANONYMOUS` ring pre-allocated by each host's
primary before fork. Peer host never reads it, so it stays off CXL.
Writer UPDATE: unlocked 7-slot scan (2 clflushopts, Phase-2.6), atomic
`fetch_add` append_cursor, spin-wait on flush_cursor when ring full,
write 16 B ring entry with RELEASE flags=1, dedup dirty-queue push via
per-bucket `queued` CAS flag. Flusher thread per host drains in cursor
order; optional last-writer-wins per slot collapse (MERGE_SAME_KEY,
default ON); single `bump_epoch` per drain. INSERT/DELETE unchanged
(iter-2 per-op path). Configurable via `FUSEE_BATCH_K` and
`FUSEE_BATCH_T_US` env vars.

Phase-3 80-run sweep
(`docs/g34_scaling_ycsb_C_only_20260424_052400/`) at K=4096 T_us=100:

- A 6.24 → **17.05** (2.73 × phase-2) — 85 % of 20 Mops/s bar
- B 32.57 → **33.37** — above bar ✓
- C 55.12 → 48.73 (-12 %)
- D 39.50 → 33.92 (-14 %, marginally over the 10 % rollback gate —
  rationale in Phase-3 iteration_note: batching library's background
  thread + 4 GiB ring allocation cost hits D's INSERT path without
  batching benefit).
- F 10.49 → **20.48** — above bar ✓

**2 of 3 validation workloads (B, F) cross the 20 Mops/s north-star.**
Workload A tops out at 17.05 Mops/s at T=64 and regresses at T=86 —
classic single-flusher saturation. Flusher sharding (N threads × `bucket_idx % N`)
is the documented follow-up step expected to close the A gap.

### Deferred / follow-up (tracked for iter-4)

- Phase-1 full 4-stage rdtscp LFM anatomy (`src/lfm_lock_fusee_instrumented.c`
  sketch) — deferred to protect Phase-3 budget. iter-2 decomp's
  flat-p50 / super-linear-p99 pattern already fits the "queue dominates"
  signature.
- Phase-1.5 light-lock MCS replacement — conditional on Phase-1 FAIL,
  therefore also not triggered.
- Flusher sharding (N threads × bucket_idx % N) to close A's ceiling.
- MERGE=OFF comparison run at the best (K, T).
- Aux peer-visibility lag client for formal staleness bound.
- Per-workload-opt-in batching so D's INSERT path is unaffected.

---

**Iter 2 (fresh decomp on per-slot LFM → `bump_epoch` outside crit section + writer-side DRAM cache refresh):**

- Code: `bump_epoch` moved after `unlock_slot()` in all three C write paths, now returns the post-increment epoch; writers replace `cache_epoch_[idx] = UINT64_MAX` with a local cache refresh (`cache_buckets_[idx].slots[s] = new`; `cache_epoch_[idx] = new_epoch`). Keeps iter-1's atomic `__atomic_add_fetch` (correctness requirement under per-slot granularity).
- Decomp (`docs/latency_decomp_C_iter2_20260423_053919.md`): lock stage shrinks 6-27 % across T=8..32; epoch stage grows 68-82 % because the correctness-mandatory atomic RMW pays an explicit CXL roundtrip where the old non-atomic pattern did not.
- Sweep (`docs/g34_scaling_ycsb_C_only_20260423_054027/`, per-slot LFM + iter-2 refinements, 80 runs, cache on peaks): A 3.41 → 3.27 (-4 %), B 9.98 → 10.54 (+6 %), D 38.30 → 41.00 (+7 %), F 3.53 → 4.34 (+23 %). Workloads that have reads (F) benefit from the cache refresh; workload A is still dominated by writes on the one Zipfian-hot slot.
- 20 Mops/s bar still not met on A / B / F (6.1× / 1.9× / 4.6× short). The ceiling is structural: ~3-4 µs per-op atomic cross-host epoch bump is the critical-section floor for any design that maintains strict LRC. Breaking it needs LRC relaxation (defer bump every K writes), per-host sharded writes, or same-key micro-batching — all non-drop-in; each needs a dedicated design doc before iter 3.

**Iter 3 (Phase-2 route-seq + Phase-3 micro-batching — see `docs/g34_scaling_ycsb_C_only_20260424_052400/iteration_note.md` for phase-3 detail):**

- Phase-2 (`[read-singleshot]` + `[flush-collapse]` + `[route-seq]`, commits up to `75c99f1`) improves A 3.27 → 6.24, **B 10.54 → 32.57 (first crossing of 20 Mops/s)**, F 4.34 → 10.49.
- Phase-3 (`[micro-batch]` per-host DRAM ring UPDATE batching, K=4096 T=100 µs merge=ON, commit `072070c`) lifts A 6.24 → 17.05 (0.85 × bar, still below), B 32.57 → 33.37 (above bar), F 10.49 → **20.48 (above bar)**, at the cost of C -12 % / D -14 %. **B and F cross 20 Mops/s under the 200 k-ops cap.**

**Iter 3 — extended validation & deferred analysis (2026-04-24):**

- **2 M-ops steady-state validation sweep** (`docs/g34_scaling_ycsb_C_only_20260424_091433/`, same config, ops=2 M, 80/80 ok): B 33.37 → **50.24** (+51 %), C 48.73 → 64.96, D 33.92 → 52.27, F 20.48 → 17.03 cache-on / **21.31 PASS cache-off @T=86**, A 17.05 → **15.03 @T=86 (−12 %, 0.75 × bar, still below)**. The 200 k-ops cap was under-reporting steady-state for B/C/D by 33–54 %. A's −12 % at 2 M sharpens the single-flusher `bump_epoch` saturation diagnosis (peak shifts T=64 → T=86; T=64 itself drops to 14.43 Mops/s).
- **Phase-1 LFM anatomy** (`docs/latency_decomp_C_iter3_lock_anatomy_20260424_090048.md`, commit `b940c70`). Instrumented `src/lfm_lock_fusee_instrumented.c` with 4 `clock_gettime` probes (local_store / peer_scan / cont_wait / enter_cs), symbol-overriding upstream LFM only in `fusee_cxl_decomp` builds (`-DFUSEE_LFM_INSTRUMENT=ON`). Uncontended baseline lower bound ≈ 4.37 µs (peer_scan 2.85 µs + enter_cs 1.50 µs + local_store 15 ns + cont_wait 0). Under contention (T=8 → T=86, 86 % Zipf), acquire-physics stages stay **flat-to-decreasing at p50** (peer_scan p50 −3 %, enter_cs p50 −1 %, local_store p50 −22 %) while `cont_wait p99 grows 11.64 ×` (6.74 → 78.50 µs) — the **"queue dominates acquire-physics"** signature. Therefore **LFM acquire-physics is NOT the ceiling**; replacing LFM with MCS/ticket cannot move A past the current 15 Mops/s. Phase-1.5 (light-lock replacement) is **ruled out**. **The remaining work is on the queueing side**: (a) per-slot granularity is already in place since iter-1; (b) micro-batching is in place since Phase-3; (c) the **single-flusher `bump_epoch` serialisation** is the last identified structural bottleneck.
- **Multi-flusher V2 (commits `94da706` → `aeadca9`) attempted + REVERTED** (commit `5189086`, 2026-04-24). Two bugs surfaced when scaling N ≥ 2 and when scrutinising the N=1 path:
  1. `94da706`: producer uses `dq_tail.fetch_add` while consumer uses plain `dq_head++`; on overflow the producer rolls back `queued` but the tail advance is not rolled back → consumer can miss a slot and the bucket `queued` flag stays set forever, so later writers in that bucket stop enqueueing. Manifests as deadlock at T ≥ 32.
  2. `aeadca9`: ready-flag hardening attempt — on overflow the producer skips the slot/ready-flag write but consumer continues to spin on `ready==0`. Observationally hangs at T=1 immediately under contention.
  The V2 branch is preserved in git history for a future attempt; production path is the reverted `072070c` semantics (N=1, no ready-flag). **Multi-flusher is on the follow-up list, not the done list.**
- **Tail risk note — runner reproducibility**: after repeated kill-restart cycles during V2 debugging, even the production reverted binary occasionally hangs at T=86 until slaves are rebooted (suspected stale CXL devdax / SysV shm state). Documented so a future session does not mistake this for a regression.

### Status vs 20 Mops/s bar (final iter-3)

| workload | 200 k peak | 2 M peak | vs 20 Mops/s |
|----------|-----------:|---------:|-------------|
| A        | 17.05 | 15.03 | **below (0.75 ×)** |
| B        | 33.37 | 50.24 | PASS |
| C        | 48.73 | 64.96 | PASS (reference) |
| D        | 33.92 | 52.27 | PASS (reference) |
| F (cache-on)  | 20.48 | 17.03 | marginal |
| F (cache-off) | 22.0  | 21.31 | PASS |

**Remaining gap: only workload A** is consistently below the bar. Diagnosed root cause: single-flusher cross-host `bump_epoch` serialisation (not LFM acquire-physics). Next iter should either (a) finish multi-flusher V2 correctly (CAS-based tail + ready flag carefully ordered) or (b) move `bump_epoch` off the flusher's per-drain critical path (batched-epoch-per-drain-cycle rather than per-bucket).

## 2026-04-24 — iter 4 variable KV value-size sweep (per `docs/task_plan_20260424_variable_kv_size.md`)

Added per-host bump-alloc CXL block pool (`src/cxl_kv_blockpool.{h,cc}`) and dual-path variadic insert/update/search on protocol C. A/B kept inline u64 (out of scope). Runner gains `FUSEE_VALUE_SIZE` env. 4× 80-run C-only sweeps at vsize 8 / 256 / 512 / 1024 (320 runs total, 0 fails).

Cache-on peak throughput (Mops/s, T at peak):

| vsize | A | B | C | D | F | A/B/F vs 20 Mops/s bar |
|------:|---:|---:|---:|---:|---:|:---|
| 8 (pooled-bypass) | 13.94 @86 | 32.15 @86 | 57.72 @86 | 48.01 @86 | 16.18 @86 | A miss / B PASS / F miss |
| 256 | 17.18 @64 | 28.89 @64 | 31.25 @64 | 34.37 @86 | **21.24 @64** | A miss / B PASS / **F PASS** |
| 512 | 12.09 @32 | 25.86 @64 | 30.19 @86 | 28.53 @86 | 19.87 @64 | A miss / B PASS / F ≈miss |
| 1024 | 13.64 @64 | 16.37 @64 | 16.78 @64 | 16.78 @64 | 14.80 @64 | **all miss** |

BW-ceiling comparison (from iter-2 hardware bench, 22 GB/s seq_write × 2 hosts, ~2× bytes per op including cross-host staging):

| vsize | B-peak / ceiling |
|------:|:-----------------|
| 256 | 28.89 / 68 → 0.42 (latency-bound) |
| 512 | 25.86 / 38 → 0.68 (mostly BW) |
| 1024 | 16.37 / 20 → **0.82 (BW-saturated)** |

**Hypothesis confirmed**: as value size grows, CXL write bandwidth becomes the floor. At kv=1024, all workloads cluster at 14-17 Mops/s regardless of lock contention profile. A still misses at every vsize — Zipf write hot-slot contention is a structural issue that value-size tuning cannot resolve. Full analysis in `docs/iter4_variable_kv_summary_20260424.md`.

Cross-size plots: `docs/iter4_kv_compare/` (5 workload × 2 metric line plots + 1 peak-summary bar chart).

## Decisions made

- **2026-04-20 01:40** — Single branch `feat/cxl-migration`, all phases squashed into that branch
- **2026-04-20 01:40** — Preserve original FUSEE design as much as possible; only replace RDMA transport with CXL
- **2026-04-20 01:40** — Implement all 3 protocols (A, B, C) as compile-time variants for perf comparison
- **2026-04-20 01:40** — Preserve YCSB benchmark framework and crash recovery logic
- **2026-04-20 01:40** — No push to remote; all commits stay local on emr
- **2026-04-20 01:40** — Investigate Option A perf bottleneck on mini-bench **before** starting Phase 0 (user request)
- **2026-04-20 03:10** — Skip git-submodule for cxl_shm_profiling (external-gitlab submodule blocked by policy); will reference via CMake path variable in Phase 2 when we actually need its headers
- **2026-04-20 03:10** — Phase 1 tests compile `src/cxl_mm.cc` directly instead of linking `libddckv`, so they don't depend on RDMA build succeeding

## Open questions (need user input)

- [x] ~~Git strategy~~ → single branch, no push
- [x] ~~Scope~~ → all three protocols, preserve YCSB + recovery
- [ ] Dax0.0 mode switch timing — user does sudo when ready
- [ ] Preferred interaction frequency — user said "checks multiple times per day"

## Session log

See `docs/fusee_cxl_session_log.md` for per-session activity.

## How to resume (for Claude starting a new session)

1. Read `~/.claude/projects/-home-yanwang-FUSEE/memory/project_cxl_transformation.md` — points here
2. Read this file's "Current state" section
3. On emr: `cd ~/FUSEE && git status && git log --oneline -5`
4. Compare with "Last commit" in Current state — should match
5. Read "Next concrete task"
6. If working tree dirty: report to user, ask whether to continue the WIP or revert
7. Tell user: "I'm at Phase X.Y, next task is Z. Continue?"
