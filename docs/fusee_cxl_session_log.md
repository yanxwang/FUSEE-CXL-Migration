# FUSEE → CXL Migration Session Log

> Chronological log of each work session. Append-only.

---

## Session 2026-04-20 ~01:40 CDT — Planning / setup

**Context**: user re-scoped the work. We actually need to modify FUSEE src/. Dev host = emr.

**Done**:
- SSH'd to emr; confirmed Xeon Gold 6530 (clflushopt supported), 128 GB DRAM + 256 GB CXL
- Identified `/dev/dax0.0` currently in system-ram mode (blocker for CXL testing)
- Synced `/home/yanwang/cxl_shm_profiling/` → `emr:~/cxl_shm_profiling/`
- Built `bench/measure_latency` on emr, validated DRAM latencies (332 ns STORE, 535 ns LOAD)
- Could not test CXL latency (dax0.0 not devdax yet; sudo reconfigure blocked)
- FUSEE on emr is at `~/FUSEE`, main branch, 2 commits (initial commit + extended FAST paper)
- Untracked user files: localhost_configs/, run_simple_test.sh, etc. — not touching these
- Created `docs/fusee_cxl_progress.md` (this session started it)
- Set up session log file (this one)

**Not done / deferred**:
- CXL devdax mode switch (user sudo needed)
- Actual FUSEE modifications (haven't started Phase 0)
- Option A deep-dive analysis (will do before Phase 0 per user request)

**Ends with**: investigation plan for Option A perf bottleneck (in progress)

---

## Session 2026-04-20 ~02:30 CDT — Option A side-track + Phase 1

**Context**: user promoted the Option A perf investigation to an ongoing side-track with its own research log, then we moved on to Phase 1.

**Option A side-track (commit 7fa6f34)**:
- Created `docs/option_a_perf_analysis.md` (first investigation report)
- Created `docs/option_a_side_track.md` (long-term research log for future A improvements)
- A-v2 already integrated into `cxl_shm_profiling/bench/ycsb_abc_bench.c` (SPSC ring replaces per-bucket scan, 180x write latency improvement, remaining ~2x gap vs B/C is inherent sync-ACK cost)
- Ideas parked for later: packed ring entry (tried, reverted), write pipelining, batch ACKs, quorum ACK, NUMA-aware placement

**Phase 1 (commits a3e7a63, 60433be)**:
- Decided not to add cxl_shm_profiling as a git submodule (policy blocks external-gitlab submodules); will reference via CMake path when needed in Phase 2
- Wrote `src/cxl_mm.{h,cc}`: `cxl_region_init` (open+ftruncate+mmap, handles EINVAL on devdax, rounds up to 2 MiB) and `cxl_region_destroy`
- Wrote `tests/cxl_mm_test.cc` (single-proc magic-word sanity) — passes on /dev/dax0.0 and on tmpfs scratch file
- Wrote `tests/cxl_mm_mp_test.cc` (fork both procs, each calls `cxl_region_init` independently, clflushopt-based handshake) — passes on /dev/dax0.0 and on tmpfs
- Registered `cxl_mm.cc` in `libddckv` source list for later phases; Phase 1 tests bypass libddckv by compiling `cxl_mm.cc` directly (libddckv still has RDMA deps, to be refactored in Phase 4)

**Discovered / noted**:
- emr user is `wang`, not `yanwang`; cxl_shm_profiling lives at `/home/wang/cxl_shm_profiling/` on emr (progress doc had wrong path earlier)
- The shell-heredoc-over-ssh commit message pattern truncates at unescaped parens in body text; Phase 1.2 commit body lost the last two lines (non-critical)

**Ends with**: Phase 1 complete (both commits land on `feat/cxl-migration`). Ready for Phase 2 (BucketLock table).

---

## Session 2026-04-20 ~03:30 CDT — Phase 2: BucketLock table

**Context**: continuing straight into Phase 2 in auto mode. Goal: plant a RACE-bucket-aligned lock table on CXL, wrap cxl_shm_profiling LFM, verify mutual exclusion across processes.

**Decisions**:
- Skip FUSEE/external/ submodule. CMake variable `CXL_SHM_PROFILING_DIR` (default `~/cxl_shm_profiling`) gives Phase 2+ tests include access to the LFM header + `locks/lfm_lock.c` source. Test binaries compile the LFM .c file directly rather than linking a prebuilt lib.
- Use static offsets inside the CXL region (not `shm_enable`/`shm_malloc_id` from global_allocator). FUSEE knows all region consumers up front; simple static layout wins.
- `BucketLockEntry` starts as just `{ shm_mutex_t mutex; }`. Protocol-specific fields (write_epoch for C, staging_scratch for B, ring metadata for A) get added in their respective phases without changing the lock API.

**Done (commit c1f40e6)**:
- `src/cxl_bucket_lock.{h,cc}`: `BucketLockTable::attach(base, num_buckets, init_mutexes)`, `lock(idx, host_id, num_hosts)`, `unlock(idx, host_id)`, `bytes_for(num_buckets)`. Thin view; does not own storage.
- `tests/cxl_bucket_lock_test.cc`: fork+contention — two procs, shared counter under a single bucket lock, verifies `counter == iters * 2`. Zero lost increments on /dev/dax0.0 at 50k iters/proc. ~7.4 μs per critical section on CXL end-to-end.
- CMake: switched project to `LANGUAGES C CXX`, added `CXL_SHM_PROFILING_DIR` cache var + existence warning.

**Noted for Phase 3**:
- LFM critical section alone is ~7.4 μs on CXL. That sets the floor for any KV op that takes the bucket lock. Matches the analytical budget in the side-track doc.
- shm_mutex_t is multi-cacheline (magic + x + y + b[MAX_HOST_NUM] + ready[MAX_HOST_NUM] + done[MAX_HOST_NUM] = 1 + 2 + 3*4 = 15 cachelines per bucket on MAX_HOST_NUM=4). At ~256 KiB per 16k buckets — fine, but not free. Keep in mind when sizing the region.

**Ends with**: Phase 2 complete. Queued Phase 3 (Option C KV ops) as the next concrete task; explicitly decided not to touch the existing `src/client*.{h,cc}` or `src/hashtable.{h,cc}` files yet.

---
