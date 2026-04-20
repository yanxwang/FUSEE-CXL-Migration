# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phases 1–3 and 7 (A, B, C protocols) all COMPLETE; Phase 4 (RDMA removal) next
- **Phase**: 0 skipped; 1,2,3,7 done; 4,5,6,8 pending
- **Branch**: `feat/cxl-migration` on emr
- **Last commit**: `360da05 [Phase 7] Compile-time protocol switch + single-proc micro-bench`
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
| 4: Remove RDMA deps | ⏳ pending | — | Need to split libddckv or gate RDMA files behind CMake option |
| 5: Single-node YCSB | ⏳ pending | — | YCSB runner using new API |
| 6: OpLog + crash recovery | ⏳ pending | — | Port `client_cr.cc` |
| 7: Options A and B | ✅ done | bf690a3, ea9599b, 360da05 | `cxl_kv_ops_A.{h,cc}` + `cxl_kv_ops_B.{h,cc}` + protocol switch header + bench binaries |
| 8: Performance benchmarks | 🚧 partial | 95bb1b2 | g3 tmpfs mini-bench + plot; emr CXL multi-proc bench run (Option A hangs — logged in side-track) |

## Recent results — emr /dev/dax0.0, multi-proc, 4 hosts × 2000 ops

| opt | wr   | agg thpt (kops/s) | w_avg μs | w_p99 μs | r_avg μs | r_p99 μs |
|-----|-----:|------------------:|---------:|---------:|---------:|---------:|
| A   | *    | **hangs** (multi-proc bench bug, correctness test OK at 2h×300 ops) |
| B   | 0.00 |               847 |        — |        — |     4.64 |     5.35 |
| B   | 0.50 |               371 |    14.58 |    16.21 |     4.90 |     5.96 |
| B   | 1.00 |               234 |    14.94 |    16.48 |        — |        — |
| C   | 0.00 |               851 |        — |        — |     4.17 |     4.56 |
| C   | 0.50 |               706 |     4.81 |     5.81 |     4.77 |     5.74 |
| C   | 1.00 |               607 |     5.15 |     5.87 |        — |        — |

Takeaway in the current (no reader-cache) port: **Option C dominates on writes** because Option B pays to push invalidations that nothing is subscribed to — the eager-push cost is ~10 μs/op of pure overhead. Once a reader cache lands, B should pull ahead on read-heavy mixed workloads. See `docs/fusee_mp_bench.png`.

## Next concrete task

**Phase 4 — Soft RDMA gating in CMake**:

The full Phase-4 plan is "delete nm.{h,cc}, ib.{h,cc}". Intermediate goal: add a CMake option `-DCXL_ONLY=ON` that drops all RDMA sources from libddckv and lets `make` complete without any ibverbs/RDMA headers present. libddckv with `-DCXL_ONLY=ON` becomes a pure CXL library; tests that depend on RDMA path stay gated out. Full source deletion waits until we are sure we will not need the RDMA path again.

**Follow-up investigations (deferred)**:
- Option A multi-proc bench hang (4 hosts × 100 ops times out). Correctness test at 2 hosts × 300 ops works. Something in the bench-specific path (populate-phase inserts before the barrier?) deadlocks the SPSC ring. New idea logged in `docs/option_a_side_track.md` §Idea 6.
- Add reader-side bucket cache so Option B's eager push buys something.

Guardrail: do not touch `src/client*.{h,cc}` or `src/hashtable.{h,cc}` yet — those remain RDMA-only until Phase 4. The new cxl_* files live alongside them.

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
