# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phase 1 ready to start (Option A perf investigation COMPLETE)
- **Phase**: 0 skipped; Phase 1 pending
- **Branch**: `feat/cxl-migration` created on emr (no commits yet on this branch)
- **Last commit**: `d1e9932 initial commit` (upstream, branch parent)
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
| 0: Baseline | ⏳ pending | — | emr FUSEE build, cxl_shm_profiling build, bench sanity run |
| 1: CXL basic infra | ⏳ pending | — | `src/cxl_mm.{h,cc}`, init region, unit test |
| 2: BucketLock table | ⏳ pending | — | `src/cxl_bucket_lock.{h,cc}` |
| 3: KV ops Option C | ⏳ pending | — | `src/cxl_kv_ops_C.cc` |
| 4: Remove RDMA deps | ⏳ pending | — | Delete nm.{h,cc}, ib.{h,cc}; update CMakeLists |
| 5: Single-node YCSB | ⏳ pending | — | YCSB runner using new API |
| 6: OpLog + crash recovery | ⏳ pending | — | Port `client_cr.cc` |
| 7: Options A and B | ⏳ pending | — | compile-time switch `-DCONSENSUS_OPT=A\|B\|C` |
| 8: Performance benchmarks | ⏳ pending | — | YCSB on all three protocols, compare to mini-bench |

## Next concrete task

**Phase 1 — CXL basic infrastructure**:

1. Add `cxl_shm_profiling` as git submodule under `FUSEE/external/cxl_shm_profiling/` (emr has it at `~/cxl_shm_profiling/` already)
2. Create `src/cxl_mm.h/cc`:
   - `cxl_region_init(dev_path, size)` — open + ftruncate-if-possible + mmap
   - `cxl_region_destroy(region)` — munmap + close
   - Uses `PROT_READ | PROT_WRITE | MAP_SHARED`, handles EINVAL on ftruncate for devdax
   - Round up size to 2 MB alignment for devdax compat
3. Create minimal test `tests/cxl_mm_test.cc` — mmap a region, write magic, read back
4. Update `CMakeLists.txt`: add cxl_shm_profiling subdir, link libglobal_allocator
5. Commit: `[Phase 1.1] CXL mm skeleton + unit test`

**Dax0.0 ready**: ✅ reconfigured to devdax mode on 2026-04-20 02:00 CDT
**Build status**: original FUSEE may not build (RDMA deps); we'll fix in Phase 4 when we remove RDMA

## Decisions made

- **2026-04-20 01:40** — Single branch `feat/cxl-migration`, all phases squashed into that branch
- **2026-04-20 01:40** — Preserve original FUSEE design as much as possible; only replace RDMA transport with CXL
- **2026-04-20 01:40** — Implement all 3 protocols (A, B, C) as compile-time variants for perf comparison
- **2026-04-20 01:40** — Preserve YCSB benchmark framework and crash recovery logic
- **2026-04-20 01:40** — No push to remote; all commits stay local on emr
- **2026-04-20 01:40** — Investigate Option A perf bottleneck on mini-bench **before** starting Phase 0 (user request)

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
