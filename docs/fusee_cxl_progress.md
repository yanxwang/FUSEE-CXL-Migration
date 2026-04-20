# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phases 1–7 (all 3 protocols + CXL_ONLY build + YCSB runner + OpLog) done; Phase 8 is the ongoing benchmark polish
- **Phase**: 0 skipped; 1,2,3,4,5,6,7 done; 8 in progress
- **Branch**: `feat/cxl-migration` on emr
- **Last commit**: `bd93e82 [Phase 6] CXL OpLog for crash recovery + v2 bench sweep results`
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
| 6: OpLog + crash recovery | ✅ done (basic) | bd93e82 | `cxl_oplog.{h,cc}` per-host ring + begin/commit/abort + recovery scan; integration into each protocol deferred |
| 7: Options A and B | ✅ done | bf690a3, ea9599b, 360da05, ff4d32d | A + B + protocol switch; A multi-proc robustness fix (always-clear op_id) |
| 8: Performance benchmarks | 🚧 partial | 95bb1b2, 75996a9, bd93e82 | g3 tmpfs mini-bench; emr CXL multi-proc bench v1/v2; Option A wr=1.0 still has stall issues |

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

## Next concrete tasks

Phases 1–7 have landed; what is genuinely left is not "new code for new phases" but hardening / extending what is there:

1. **Fix Option A multi-proc stall at wratio=1.0** — with the always-clear fix, 4-host × 500 ops now completes, but host 3 regularly sits on 600 ms ACK tails. Root cause still unpinned. Candidate investigations: instrument *which* dst is not ACKing and why the replicator head is not advancing on that dst.
2. **Wire OpLog into the three CxlKvStore classes** — begin()/commit() around insert/update/remove. Writer holds the bucket lock during the op so there is no contention concern; the question is just where in the call sequence the log calls go.
3. **Add reader-side bucket cache for Option B** — today B pays the eager-push cost for no benefit, because readers always go to CXL. Caching + push-driven invalidation is the scenario where B is supposed to win.
4. **Full YCSB using the official workloads** — right now `tests/gen_ycsb_spec.py` emits uniform-keyed synthetic spec files. Real YCSB uses Zipf skew. Hook in the official workload downloader (already in setup/) or port the Zipf generator.
5. **Phase 4 hard removal** — actually delete `src/nm.{h,cc}`, `src/ib.{h,cc}`, and the RDMA-only tests once we are sure the CXL-only path is the permanent one.

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
