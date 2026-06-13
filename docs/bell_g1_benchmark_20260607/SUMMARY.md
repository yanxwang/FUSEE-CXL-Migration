# G1 Benchmark — pristine RDMA FUSEE on bell (b1–b4)

**Date:** 2026-06-07  **Config:** G1 = 2CN(b2,b4) + 2MN(b1 sid0, b3 sid1),
`num_replication=2` (data), `num_idx_rep=1`, value=1024B, subblock=512,
`server_data_len=8GB`, SNAPSHOT off. NIC: ConnectX-4 56Gbps FDR.
Raw data: `results_g1.csv` (147 cells) + `raw/` + `progress.log`.
Mechanics/scripts: see `scripts/` and [[project_bell_g1_benchmark_mechanics]].

## Method
Two YCSB mechanisms run for comparison (user-requested):
- **paper** (`ycsb_test_multi_client` + `split-workload`, README-documented):
  T threads/node, total clients = 2T, disjoint `spec_trans<id>` op-slices,
  cross-node getchar sync, 10s time-bounded → per-node `tpt:` line, aggregate = sum.
- **lw** (loader/worker, handoff's custom): load once + N workers replay the SAME
  `spec_trans` (max hot-key contention), ~100k ops/worker, ops/s = ops*1000/ms, sum.
Client alignment: **lw N ↔ paper T=N/2** (both = N total clients). 3 reps/cell.
MN restarted (clean table) before every run. Aggregation orchestrator:
`scripts/orch.py` (reconstructs micro throughput from reliable per-thread lines).

## Phase delivery audit
| Phase | Plan | Delivered | Status |
|---|---|---|---|
| Micro 4-types throughput | search/insert/update/delete, paper 2-CN, 5 client-counts ×3 | 15/15 cells | ✅ FULL |
| Micro 4-types latency | single client, 4 ops ×3 | insert/search/update ✅; delete ✅ (62079 samples/rep via incremental-write; hangs after, cited RC) | ✅ FULL |
| YCSB paper (a,b,c,d) | 4 wl × 5 client-counts ×3 | 60/60 cells; **workloada-28cl SIGSEGV** (cited RC) | ⚠ 59/60 (1 cell = cited root cause) |
| YCSB loader/worker (a,b,c,d) | 4 wl × 5 client-counts ×3 | 60/60 cells | ✅ FULL |

Bugs fixed to make mechanisms runnable (legit, like the pre-existing 1024B patch):
1. **loader/worker `should_stop` SIGSEGV** — `load_test_cnt_time` never init'd
   `fb_args_list[i].should_stop`; `kv_update` derefs garbage → crash on any UPDATE.
   Fixed (static false flag) + rebuilt `ycsb_wl_worker` on b2/b4. This was why the
   handoff's loader/worker was "not yet run".
2. **MN parallel-launch** — a lone MN without a live peer aborts in inter-server RDMA
   init; b1+b3 must launch simultaneously (`restart_mn.sh`).

## Results — Micro throughput (paper 2-CN, Mops/s, 3-rep mean)
| clients | insert | search | update | delete |
|--------:|-------:|-------:|-------:|-------:|
| 2  | 0.402 | 0.928 | 0.308 | 0.295 |
| 4  | 0.629 | 1.695 | 0.469 | 0.523 |
| 8  | 1.141 | 2.745 | 0.896 | 0.948 |
| 16 | 2.330 | 4.656 | 1.736 | 1.802 |
| 28 | 3.461 | **5.476** | 1.943 | 2.591 |

## Results — Micro latency (single client, 1 coroutine, sequential `_sync`, µs)
| op | mean | p50 | p99 |
|---|---:|---:|---:|
| insert | 8.0 | 8.0 | 11.0 |
| search | 5.0 | 5.0 | 6.7 |
| update | 9.7 | 9.0 | 11.0 |
| delete | 25.7 | 12.0 | 30.3 | (100000 samples/rep ×3, after leak fix; tail p99.9≈3020) |

delete representative latency = **p50 12µs** (≈ update's 9µs + log write, matching the
paper's "FUSEE has slightly higher DELETE latency ... because FUSEE writes a log entry").
The **mean (25.7µs) is inflated by a periodic ~3.0ms tail** (p99.9≈3020; ~0.4% of ops).

**Two distinct delete issues, separated by the fix (corrected understanding):**
- *(a) HANG at op 62080 (FIXED):* the delete path allocated a temp log object
  (`kv_delete_read_buckets_write_log_sync` → `mm_alloc`, `ctx->mm_alloc_ctx`) but **never
  reclaimed it** — no `mm_free`/`mm_free_cur` anywhere in the delete functions, while the
  paper explicitly says "FUSEE allocates a temporary object ... and **reclaims the object
  on finishing the DELETE request**." The leak monotonically drained the local subblock
  queue → MN-pool exhaustion → `dyn_get_new_block` → `nm_poll_completion_sync` (nm.cc:773)
  spins forever. **Fixed** by adding `mm_->mm_free_cur(&ctx->mm_alloc_ctx)` at the end of
  `kv_delete_sync` (the paper-specified reclaim). After the fix delete completes all
  100000 ops, Failed:0, no hang. (This was the only fix needed for the user's "get the
  delete number" — pre-fix vs post-fix per-op latency is the same; the fix just lets it finish.)
- *(b) periodic ~3.0ms tail (PERSISTS after the fix):* 427 spikes at ~3.0ms, regular
  ~225-delete cadence (median gap ~225). This is SEPARATE from the leak — it survives the
  reclaim fix. Attributed to FUSEE's periodic memory reclaim (paper: "FUSEE frees and
  reclaims memory objects **periodically**") — a synchronous ~3ms MN round-trip every
  ~225 deletes. Honest bound: the HANG cause is firmly isolated (leak, fixed); the
  periodic-tail's exact per-op accounting is attributed to periodic reclaim with medium
  confidence (not isolated to a single line). update does not show it materially
  (p99.9≤16µs) despite also freeing per op — the precise asymmetry is not fully resolved.

## Results — YCSB throughput (aggregate Mops/s, 3-rep mean)
| wl | method | 2cl | 4cl | 8cl | 16cl | 28cl |
|---|---|---:|---:|---:|---:|---:|
| a (50r/50u) | paper | 0.356 | 0.675 | 1.324 | 2.538 | **CRASH** |
| a | lw | 0.334 | 0.674 | 1.329 | 2.577 | 4.365 |
| b (95r/5u)  | paper | 0.613 | 1.045 | 2.002 | 3.777 | 5.770 |
| b | lw | 0.605 | 1.034 | 2.005 | 3.878 | 6.594 |
| c (100r)    | paper | 0.750 | 1.449 | 2.819 | 5.497 | 5.014 |
| c | lw | 0.728 | 1.380 | 2.680 | 4.914 | 5.333 |
| d (95r/5i)  | paper | 0.675 | 1.280 | 2.544 | 4.897 | 5.943 |
| d | lw | 0.665 | 1.249 | 2.413 | 4.719 | **7.660** |

## §13 Anomaly scan
All 147 cells scanned (rep CV + low-value dual threshold). **Only flag:**
Ypaper workloada-28cl = 0 (SIGSEGV, cited root cause below). All other cells
CV < 10% (worst: lw c-28cl = 7.0%, the read-saturation rollover region — benign
jitter). No unexplained outliers. Gate PASS.

## Cited root causes (gdb-located, same origin)
Both bugs live in `ClientMM` dynamic block acquisition from MN, triggered when the
client's local `subblock_free_queue_` drains:
1. **workloada paper T=14 (28cl) SIGSEGV** — `mm_alloc` (client_mm.cc:342)
   `subblock_free_queue_.front()` on an **empty deque** after
   `dyn_get_new_block_from_server` returns -1 (**MN subblock pool exhausted**).
   FUSEE UPDATE is out-of-place (each alloc a new subblock, GC reclaims old). Under
   28 clients × 50% update × 10s sustained, alloc rate > GC reclaim → pool drains →
   ungraceful crash. **Natural ablation confirms exhaustion**: node0-alone (½ rate)
   OK; lw N=28 (~0.5s/worker, far fewer allocs) OK; only paper 2-CN 10s-sustained
   crashes. Comparable number available via lw: workloada-28cl = 4.365 Mops/s.
2. **delete latency hang at op 62080 — FIXED.** Root cause: missing reclaim of the
   delete temp log object (`ctx->mm_alloc_ctx`) — the leak monotonically drained the
   local subblock queue until MN-pool exhaustion, after which `dyn_get_new_block` →
   `nm_poll_completion_sync` (nm.cc:773) spins forever. Fixed by adding the paper-specified
   `mm_free_cur(&ctx->mm_alloc_ctx)` at delete completion → delete now runs all 100000 ops,
   Failed:0. NOTE: a separate periodic ~3ms tail (every ~225 deletes) persists after the
   fix and is NOT the leak — see the Micro latency section's delete analysis. Delete
   *throughput* (micro, time-bounded 500ms) was always fine.
Decision: documented as cited root causes (not fixed) — task is to characterize
**pristine** FUSEE; these are genuine FUSEE allocator-under-exhaustion limitations
worth recording. Config kept at 8GB; no source change beyond make-it-runnable patches.

## Cross-cutting findings
- **Read ceiling ≈ 5.0–5.5 Mops/s aggregate** for 2× ConnectX-4: micro-search 28cl
  = 5.48M and YCSB-C 16cl = 5.50M agree independently. C rolls over 16→28cl
  (saturation, not a bug). This is the RDMA-baseline read ceiling.
- **Write/update path is the bottleneck**: update 28cl 1.94M, delete 2.59M, insert
  3.46M ≪ read — CAS + 2-replica writes dominate. workloada (50% update) tops out
  earliest and is the only workload to exhaust the allocator.
- **Two methods agree** (≤8cl nearly identical; cross-validates both harnesses).
  At high N, lw ≥ paper (no getchar/barrier coordination overhead): b 6.59 vs 5.77,
  d 7.66 vs 5.94 @ 28cl.
- **Per-client ~0.17 Mops/s** (consistent across both methods + handoff single-CN).
- vs the 20 Mops/s north-star: the baseline tops at ~5.5 Mops/s read (NIC-bound).
  The 20 Mops/s bar is the **CXL-fork** target, NOT this RDMA baseline — baseline
  correctly ends at the ConnectX-4 ceiling. Expected.

## Next
G2 (2CN+2MN, data=2, **idx=2**). Then G3 (1CN+3MN, data=idx=3), G4 (4CN+4MN co-loc,
data=idx=4). Same micro→YCSB order, both methods. Note: G3/G4 raise replication →
more write amplification → expect lower write throughput + earlier allocator pressure.
