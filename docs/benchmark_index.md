# FUSEE CXL Migration — Benchmark Index

One-screen answer to "which phase has which perf data, and where does it live?"

Written 2026-04-20. Entries should be appended as new artifacts land.

## Per-phase measurements

| Phase | What we measure | Artifact(s) | How to reproduce |
|---|---|---|---|
| 1: CXL mm | Correctness only (magic round-trip, cross-proc visibility) | `tests/cxl_mm_test.cc`, `tests/cxl_mm_mp_test.cc` | `./build-cxl/tests/cxl_mm_test /dev/dax0.0`; same for `_mp_test` |
| 2: BucketLock | μs per critical section under contention | `tests/cxl_bucket_lock_test.cc` now prints `HOST host=... us_per_crit=...` + `RESULT bench=bucket_lock ...`. Tmpfs: 5.6 μs. CXL: 7.4 μs (historical, c1f40e6) | `./build-cxl/tests/cxl_bucket_lock_test /dev/dax0.0 50000` |
| 3: KV ops C | Correctness only (cross-read, update visibility, delete leak check) | `tests/cxl_kv_ops_C_test.cc` | `./build-cxl/tests/cxl_kv_ops_C_test /dev/dax0.0 2000` |
| 4: RDMA-free build | Build signal only (`-DCXL_ONLY=ON` succeeds) | CI-like: `cmake --build build-cxl` | re-run cmake after any source change |
| 5: Single-host YCSB | load / trans throughput per workload × protocol | `docs/fusee_ycsb_sweep.log` (tmpfs snapshot; CXL refresh pending) | `bash tests/run_fusee_ycsb_sweep.sh` |
| 6: OpLog + recovery | Scan count, redo count, key-value before/after recovery | `tests/cxl_oplog_test.cc`, `_redo_test.cc`, `cxl_kv_ops_{A,B,C}_oplog_test.cc`, `_recover_redo_test.cc` | `ctest -R _oplog_` under build-cxl |
| 7: Options A + B | Correctness across two processes + replicator ACK counts | `tests/cxl_kv_ops_{A,B}_test.cc` | `./build-cxl/tests/cxl_kv_ops_A_test /dev/dax0.0 500` |
| 8: Multi-proc perf | Per-host & aggregate throughput, w_avg/p50/p99, r_avg/p50/p99, across `{cache off/on} × {A,B,C} × {wr=0.0, 0.5, 1.0}` | `docs/fusee_mp_bench{,_v2,_v3}.log`, `docs/fusee_mp_bench_v3_cache_on.png` (and earlier v2). Per-call DRAM-cache speedup: C 10×, B 346× | `bash tests/run_fusee_mp_sweep.sh` |
| Option A side-track | Single-host A-v2 write latency vs baseline, NUM_BUCKETS sweep | `docs/option_a_perf_analysis.md`, `docs/option_a_side_track.md` | (mini-bench in cxl_shm_profiling/bench, not FUSEE build) |
| g3 tmpfs cross-check | A/B/C on different hardware (DRAM-only) | `docs/ycsb_abc_g3_tmpfs.log`, `.png` | historical; not auto-rerun |

## Scripts and plots

| Script | Purpose |
|---|---|
| `tests/run_fusee_mp_sweep.sh` | Reproducible multi-proc bench across cache/opt/wratio (Phase 8) |
| `tests/run_fusee_ycsb_sweep.sh` | Runs `cxl_ycsb_runner_{A,B,C}` against every load/trans pair in a workloads dir (Phase 5) |
| `docs/plot_fusee_v3_cache.py` | Consumes `fusee_mp_bench_v3.log`; emits 2×2 panel plot |
| `docs/plot_fusee_mp_bench.py` | Same shape for the v1/v2 logs |
| `docs/plot_g3_tmpfs.py` | Mini-bench cross-check on g3 |
| `tests/gen_ycsb_spec.py` | Synthetic Zipf/uniform spec file generator (feeds `cxl_ycsb_runner`) |

## Audit gaps still open

1. **Real YCSB workloads not committed**. `setup/download_workload.sh` pulls from Google Drive; once the tarball is fetched and unpacked into `workloads/`, re-run `WL_DIR=workloads bash tests/run_fusee_ycsb_sweep.sh` to get real-trace numbers alongside the synthetic ones.
2. **Phase 2 CXL number is historical**. The 7.4 μs number cited in commit `c1f40e6` has not been re-measured since the `BucketLockEntry` grew `write_epoch` and `staging_scratch`. Re-run `cxl_bucket_lock_test /dev/dax0.0 50000` after the next devdax reconfigure and append the result here.
3. **Multi-proc cache-off path is fragile**. `fusee_mp_bench_v3.log` historical notes mark some cache-off rows timed out; v3 plot intentionally uses cache-on only. Root cause is the same ring-full class of bug tracked in the Option A side-track.
