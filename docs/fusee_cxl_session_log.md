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

## Session 2026-04-20 ~02:00–03:50 CDT — Phases 3 + 7 (auto mode)

**Context**: user added "keep pushing until 10 AM, do not wait for phase review". Executed Phases 3 and 7 back-to-back, plus a parallel side-track bench run on g3.

**Phase 3 (commit a2fbc30)** — Option C KV ops:
- `src/cxl_hashtable.h`: 128-byte bucket with 7 inline u64/u64 slots, FNV-1a hash.
- `src/cxl_bucket_lock.{h,cc}`: extended BucketLockEntry with `write_epoch` + `staging_scratch`.
- `src/cxl_kv_ops_C.{h,cc}`: seqlock-style C (writer lock+write+epoch-bump, reader epoch-retry).
- `tests/cxl_kv_ops_C_test.cc`: two-proc insert/update/delete + cross-reads. Passes on /dev/dax0.0 at 2000 ops/host × 8192 buckets.

**libfusee_cxl (commit 4c10c3b)**: separated CXL-only sources into their own static lib so tests stop duplicating source lists and no longer implicitly depend on libddckv (which still carries RDMA deps).

**Side-track g3 bench (commit 95bb1b2)**:
- Hardware blockers: g3 dax0.0 in system-ram mode (sudo reconfigure was only authorized for emr, so left as-is); g4 has no /dev/dax* at all.
- Ran single-host multi-proc on g3 tmpfs (DRAM). 5 of 6 (opt, workload) configurations completed at 4p×2t×3000 ops. Option A at 3000 ops wl=A hangs (runs fine at 500 ops). A-v2 at 500 ops matches emr CXL numbers.
- Notable: reads A≈B at ~9.7M ops/s agg; C at ~2.0M (seqlock cost). Writes: C 9.9μs < B 13.3μs on g3 tmpfs.
- New idea 6 for side-track: A-v2 ring-full hang at higher ops (needs producer-wait-reason instrumentation).

**Phase 7A (commit bf690a3)** — Option A ported to FUSEE src:
- `src/cxl_pending_ring.h`: PendingRingEntry + PendingRingMatrix (4096 entries/ring, kMaxHosts=4).
- `src/cxl_kv_ops_A.{h,cc}`: CxlKvStoreA with replicator std::thread, dispatch_and_wait, 2s sanity ceilings on ring-full and ACK-wait paths.
- `tests/cxl_kv_ops_A_test.cc`: 2-proc correctness. Passes on /dev/dax0.0 at 300 ops/host, 239-243 ACKs observed.

**Phase 7B (commit ea9599b)** — Option B ported:
- `src/cxl_kv_ops_B.{h,cc}`: same ring data structure, writer does not wait on processed_op_id; replicator clears op_id directly.
- Passes on /dev/dax0.0 at 300 ops/host.

**Phase 7 switch (commit 360da05)**:
- `src/cxl_kv_store.h`: compile-time protocol selector via `-DCONSENSUS_OPT=FUSEE_OPT_A|B|C`. Typedefs `fusee::CxlKvStore` to the chosen class. Added `stop()` + `replicated_ops()` to CxlKvStoreC for API parity.
- `tests/cxl_kv_bench.cc` + `cxl_kv_bench_mp.cc`: unified single-proc and multi-proc benches, compiled 6 times (3 protocols × 2 variants).

**Multi-proc bench results — emr /dev/dax0.0, 4 hosts × 2000 ops/host**:
- Option A: **hangs** even at 4h × 50 ops. Correctness test at 2h × 300 ops still passes → isolated to the bench path (pre-barrier populate deadlocks 4-way SPSC ACK wait).
- Option B: agg 847 / 371 / 234 kops/s at wratio 0 / 0.5 / 1.0. Writes 14.6–14.9 μs avg, reads ~4.8 μs avg.
- Option C: agg 851 / 706 / 607 kops/s, writes 4.8–5.2 μs avg, reads ~4.2–4.8 μs avg.
- Key finding: in the current cache-less port, **C beats B on writes by ~3×** because B's eager-push buys nothing (no reader cache to invalidate).

**Ends with**: Phases 1–3 + 7 landed (8 feature commits on feat/cxl-migration). Queued Phase 4 (RDMA gating) as the next concrete task.

---

## Session 2026-04-20 ~03:50–04:40 CDT — Phases 4, 5, 6 + A multi-proc robustness

**Context**: user said "keep pushing until 10 AM, do not wait for phase review". Landed 4 more feature commits in one session.

**Phase 4 — CXL_ONLY CMake gate (commit bbf3986)**:
- Added top-level `option(CXL_ONLY "…" OFF)`. When ON the build skips Boost, GTest, the RDMA-linked tests, ycsb-test/crash-recover-test/micro-test, and libddckv entirely. Verified both ways on emr: `build/` still produces full libddckv + RDMA tests + ycsb-test; `build-cxl/` (fresh dir with `-DCXL_ONLY=ON`) produces only libfusee_cxl + the cxl_*_test binaries.
- Moved `cxl_mm.cc`, `cxl_bucket_lock.cc`, `cxl_kv_ops_C.cc` out of libddckv source list (they already live in libfusee_cxl and do not need libddckv to carry the cxl_shm_profiling include path).
- Hard RDMA source deletion is deferred; gate is the intermediate step.

**Option A multi-proc robustness (commit ff4d32d)**:
- Found: if one dst's ACK timed out in `dispatch_and_wait`, the writer returned without clearing `op_id` on *any* slot. Next writer for any of those rings blocked forever on "ring slot free".
- Fix 1: always run the `op_id=0` clear loop even on timeout.
- Fix 2: tightened the per-dst ACK budget from 2 s to 200 ms so a stuck replicator is surfaced in reasonable time.
- Fix 3: added `init_done` + `attached` barriers in `cxl_kv_bench_mp.cc` so host 1/2/3 do not race against host 0's ring-matrix memset or enqueue before the peers' replicators are running.
- Result: 4-host × 500 ops now completes for A at every wratio. But wr=1.0 on A still shows host 3 stalling at 600 ms tails repeatedly, agg thpt collapses to ~24 ops/s — real deadlock-like behavior that the always-clear only papers over. Root cause still open.

**Phase 5 — YCSB runner (commit 29ba277)**:
- `tests/cxl_ycsb_runner.cc`: parses YCSB spec files (`OP KEY` or `OP TABLE KEY`), FNV-1a hashes string keys to u64, dispatches to `fusee::CxlKvStore`. Compiled into `cxl_ycsb_runner_{A,B,C}` via the same per-protocol CMake foreach as the bench.
- `tests/gen_ycsb_spec.py`: synthesizes wl_A / wl_C spec files with uniform key distribution (5k load + 5k trans ops). Real YCSB with Zipf skew is a follow-up.
- Single-host results on /dev/dax0.0: all three protocols at ~210-220 kops/s load, ~275-315 kops/s trans. As expected — no cross-host replication when num_hosts=1, so A/B/C converge to the same lock-bound cost.

**Phase 6 — OpLog (commit bd93e82)**:
- `src/cxl_oplog.{h,cc}`: per-host ring (4096 entries each, up to `kOpLogMaxHosts=4`), `begin` / `commit` / `abort` primitives with state word published last, plus `scan_in_progress` recovery scanner that walks every host's tail and calls a visitor for each InProgress entry.
- `tests/cxl_oplog_test.cc`: host writes 3 committed ops + 1 dangling begin, then a second attach runs the scan and must find exactly 1 InProgress. Passes on /dev/dax0.0 and tmpfs.
- OpLog is not yet wired into `CxlKvStoreA/B/C::insert/update/remove`. That is a follow-up; each op site needs a `log_.begin(...)` before the store mutation and a `log_.commit(...)` after.

**v2 multi-proc bench (also in bd93e82)**:
- Rerun with the A robustness fixes + barriers. At 4 hosts × 500 ops on /dev/dax0.0:
  - Reads (wr=0): A ≈ B ≈ C, all at ~1.21 M ops/s agg.
  - Writes (wr=1): B 361 kops/s (7.2us/op), C 895 kops/s (4.0us/op) — C still ~2.5× faster than B on writes because no reader cache exists to justify B's eager-push cost.
  - A wr=1.0: agg 24 ops/s because host 3 stalls (600ms ACK tails). Logged as open.

**Ends with**: Phases 1–7 complete; Phase 8 (benchmarks) partially in tree, with a clean reproducible multi-proc setup and two rounds of results. Next concrete work is the Option A stall root-cause and reader-cache for Option B.

---
