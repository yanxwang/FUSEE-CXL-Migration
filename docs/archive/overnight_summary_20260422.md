# Overnight 2026-04-21 evening → 2026-04-22 11:59 AM CDT summary

> One-page digest of what landed on the new kernel 6.15.0. Full details
> in `docs/fusee_cxl_session_log.md` (not yet updated this session),
> `docs/fusee_cxl_progress.md`, and per-task docs under
> `docs/g34_bench/`.

## Tasks requested (and their fate)

| # | Task | Status | Where to read |
|---|------|--------|---------------|
| 1 | Implement SCAN operation | **skipped by user request** | — |
| 2 | LFM N≥3 livelock root cause + fix | **done** | `docs/g34_bench/g34_livelock_root_cause.md` |
| 2.1 | Packed-entry optimization for A | **deferred** (analysis only) | `docs/option_a_side_track.md` (2026-04-22 note) |
| 3 | A/B/C read/write latency decomposition | **done** | `docs/g34_bench/latency_decomp_analysis.md` |
| 4 | 1/2/4/8/16 multi-thread scaling on one host | **done** | `docs/g34_bench/thread_scaling_analysis.md` |
| 5 | Real cross-host A/B/C × YCSB sweep | **done** (5 workloads × 3 opts × 2 cache = 30 runs) | `docs/fusee_ycsb_g34_xhost_full_20260422.{log,png}` |
| — | Kernel 6.15.0 observations log | **done** | `docs/g34_bench/g34_kernel_new_observations.md` |

## The one surprise that matters

The overnight "LFM N≥3 livelock" was actually not LFM's fault. It was
a **fork-mode init race** in `cxl_kv_bench_mp`: CXL devdax mmap on
g3/g4 does not zero-fill, non-primary children saw stale `init_done=1`
from a previous run, skipped the barrier, and the whole bench pinned
at 99% CPU on unsatisfiable flag reads. One line of code — move
`cxl_region_init` + `memset(stats,0)` to the parent before the fork —
fixes it. All three protocols at N=4 pass cleanly now.

The same class of bug existed in `cxl_ycsb_runner` role-mode; the fix
there was adding a unique `FUSEE_RUN_COOKIE` env the orchestrator
generates per run.

## Headline numbers

**Latency decomposition on g3 (Option C write path, per-primitive)**:

| N | lock (μs) | other primitives (μs, summed) | total write (μs) |
|---|-----------|-------------------------------|------------------|
| 1 |  4.2      | 2.6                           |  6.8 |
| 2 | 17.0      | 2.4                           | 19.4 |
| 4 | 26.9      | 2.4                           | 29.3 |

Lock is the dominant phase at every N. "other primitives" (flush+fence,
store, epoch bump, unlock) stay flat.

**Multi-thread scaling on g4 (single process, N pthreads, C semantics)**:

| threads | wr=0 reads | wr=0.5 mixed | wr=1 writes | speedup @ 16 |
|---------|-----------:|-------------:|------------:|-------------:|
| 1       |  2.5 M ops/s |   201 k    | 106 k       | — |
| 16      | 36.0 M     | 3 022 k    | 1 635 k     | 14.7–15.4× |

Near-linear 91–96 % scaling efficiency. No contention because random
keys spread uniformly across 65 536 buckets.

**Cross-host YCSB (g3 + g4, 2 hosts × 1 proc, trans kops/s)**:

| workload | A off/on | B off/on | C off/on |
|----------|---------:|---------:|---------:|
| a (50/50) | 182 / 190 |  258 / 257 |  305 / 354 |
| b (95 R) | 571 / 1 235 |  607 / 1 493 |  662 / 1 249 |
| c (100 R) | 724 / **3 629** |  750 / **3 662** |  773 / 1 786 |
| d (95 R latest) | 553 / 1 242 |  617 / 1 548 |  665 / 1 296 |
| f (50 R / 50 RMW) | 246 / 267 |  321 / 362 |  435 / 503 |

Reading the table: for WRITES (workload a, f), `C > B > A` always.
For PURE READS with cache on (workload c), `A ≈ B >> C` because C
still pays one CXL epoch load per read while A/B serve purely from
DRAM on a cache hit.

## Commits worth knowing

- `ff3d03a` [task 2] fork-mode init race fix + root cause doc
- `455fa89` [task 3] primitive latency decomposition + plot
- `0f59561` [task 4] multi-thread scaling bench + 15× speedup data
- `b392647` [task 5] 12-run cross-host sweep (workload a + c)
- `d70afd5` [task 5] 30-run full cross-host sweep (a/b/c/d/f)
- `d549ba0` [docs] kernel 6.15.0 observations (mmap-no-zero,
  layout-skew, stability)

## What's still unknown / deferred

- Scan operation (task 1): deferred indefinitely by user.
- Packed-entry Option A (task 2.1): deferred after showing the
  writer-flush cost (~150 ns) is 0.4 % of A's N=2 write path. Not the
  current bottleneck.
- Whether the N≥4 under the OLD kernel was really LFM or the same
  init-race. Moot under 6.15.0.
- A self-check at attach time to catch compile-time-constant skew
  between hosts (proposed in kernel-observations doc).

## Quickstart for the next session

```bash
# If g3 or g4 got PXE-rebooted overnight:
scripts/rekey_slave.sh g3 && scripts/rekey_slave.sh g4
ssh g3 'chmod 666 /dev/dax0.0'
ssh g4 'chmod 666 /dev/dax0.0'
scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4

# Re-run the full cross-host sweep (~2 min):
bash /tmp/sweep.sh  # if saved; otherwise see b392647 commit for the inline
```

On the local machine, logs under `logs/g34_sweep_full_*/` and plots /
summary docs under `docs/g34_bench/` are the permanent home for these
data.
