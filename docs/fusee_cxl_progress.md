# FUSEE → CXL Migration Progress

> **Single source of truth** for where the migration stands. Update at start/end of every work chunk.

## Current state

- **Project name**: FUSEE CXL Migration
- **Current focus**: Phase 1 + 2 COMPLETE; Phase 3 (KV ops Option C) next
- **Phase**: 0 skipped; Phase 1 done; Phase 2 done; Phase 3 pending
- **Branch**: `feat/cxl-migration` on emr
- **Last commit**: `c1f40e6 [Phase 2] CXL BucketLock table backed by LFM`
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
| 3: KV ops Option C | ⏳ pending | — | `src/cxl_kv_ops_C.cc` |
| 4: Remove RDMA deps | ⏳ pending | — | Delete nm.{h,cc}, ib.{h,cc}; update CMakeLists |
| 5: Single-node YCSB | ⏳ pending | — | YCSB runner using new API |
| 6: OpLog + crash recovery | ⏳ pending | — | Port `client_cr.cc` |
| 7: Options A and B | ⏳ pending | — | compile-time switch `-DCONSENSUS_OPT=A\|B\|C` |
| 8: Performance benchmarks | ⏳ pending | — | YCSB on all three protocols, compare to mini-bench |

## Next concrete task

**Phase 3 — KV ops for Option C (lazy-RC, simplest protocol)**:

1. Design CXL region layout for Option C:
   - Global header (magic, num_nodes, num_buckets, offsets)
   - `BucketLockTable` (already usable from Phase 2)
   - Per-bucket write-epoch array (cacheline-padded `cacheline_u64`)
   - Authoritative bucket array (RACE hash buckets living on CXL for C; B and A will diverge)
   - KV data staging area (copy-out-then-publish pattern)
2. Port/recreate a minimal RACE hash bucket + slot layout in `src/cxl_hashtable.{h,cc}`. Start with a single subtable (no directory splits) to keep Phase 3 small.
3. Create `src/cxl_kv_ops_C.{h,cc}`:
   - `kv_insert(key, klen, val, vlen)` — lock bucket, scan for empty slot, write KV-data + slot, bump bucket write_epoch, unlock
   - `kv_search(key, klen, out)` — on cache miss (epoch changed), refresh bucket from CXL; otherwise use local cached slots
   - `kv_update(key, klen, val, vlen)` / `kv_delete(key, klen)` — similar lock+write+epoch-bump
   - Keep the local hash-table cache per node in DRAM (FUSEE's original model)
4. Multi-process test `tests/cxl_kv_ops_C_test.cc`: two processes do disjoint inserts + cross-reads; verify every insert from node A is visible on node B after its local epoch-check refresh.
5. Commit as `[Phase 3] Option C KV ops + multi-proc test`

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
