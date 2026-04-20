# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phases 1–8 all have substantive work landed; all three protocols pass multi-proc correctness, A stall fixed, cache-on v3 sweep complete
- **Phase**: 0 skipped; 1,2,3,5,6,7 done; 4 done (soft-gate, hard-delete deferred); 8 done (cache-on numbers; cache-off sweep is fragile but individual runs work)
- **Branch**: `feat/cxl-migration` on emr (32 commits)
- **Last commit**: `56fb6b7 [Phase 6] CxlKvStoreC::recover_from_oplog helper + redo integration test`
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
2. **Run a cache-on multi-proc bench sweep and generate a v3 plot** — today's numbers are from ad-hoc invocations; should be a reproducible sweep with FUSEE_CACHE=1.
3. **Phase 4 hard deletion** — delete `src/nm.{h,cc}` / `src/ib.{h,cc}` and the client/server RDMA files once we are sure the RDMA path stays gone. The soft gate is enough for now; hard deletion is intentionally deferred.
4. **Integrate OpLog recovery callback into each CxlKvStore** — partial. C now has `CxlKvStoreC::recover_from_oplog()` + `tests/cxl_kv_ops_C_recover_redo_test.cc` (written + tmpfs-green 2026-04-20 ~08:55 CDT, still uncommitted because emr is unreachable). A/B need a `recovery_mode_` short-circuit inside `dispatch_and_wait` / `dispatch_nowait` so redo does not hang spinning on peers that have not restarted yet — deferred to the next emr session.
5. **Official YCSB workloads** — Zipf generator exists, but hooking in the real `workloads/` dir from `setup/download_workload.sh` is still untouched.

Guardrail: do not touch `src/client*.{h,cc}` or `src/hashtable.{h,cc}` yet — those remain RDMA-only under the default build. The new cxl_* files live alongside them and are selected via `-DCXL_ONLY=ON` or via linking `libfusee_cxl` directly.

**Dax0.0 ready**: ✅ reconfigured to devdax mode on 2026-04-20 02:00 CDT
**Build status**: original FUSEE still has RDMA deps in libddckv; Phase 1 tests link cxl_mm.cc directly, bypassing libddckv. Full libddckv refactor in Phase 4.

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
