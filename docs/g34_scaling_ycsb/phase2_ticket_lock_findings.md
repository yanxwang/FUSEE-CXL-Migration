# Phase 2 ticket lock — findings (deferred)

**Status**: implemented but disabled by default (`FUSEE_USE_TICKET_LOCK=ON`
is off). YCSB runner hangs with ticket lock enabled; atomic microbenchmark
passes. Root cause not yet diagnosed in the allotted time.

## What was built

- `cxl_shm_profiling/locks/ticket_lock.{h,c}`: standard ticket lock,
  `next_ticket` + `now_serving` on separate cachelines + magic word.
- `src/cxl_bucket_lock.{h,cc}`: compile-time swap between `shm_mutex_t`
  (LFM, default) and `ticket_mutex_t` via `FUSEE_USE_TICKET_LOCK=1`.
- CMake option `-DFUSEE_USE_TICKET_LOCK=ON` wires the flag through.

## Microbench results — ticket lock works intra-host

`cxl_latency_decomp /dev/dax0.0 2000 N` on g3 (cross-process fork, same
host, CXL devdax):

| N | lock avg (ns) LFM | lock avg (ns) TICKET |
|---|---|---|
| 1 | 4 223 | **754**  |
| 2 | 16 959 | **3 234** |
| 4 | 26 868 | **19 216** |

Ticket wins by 1.4-5.6× at low N. The bench is limited to N ≤ 4, so the
high-contention comparison (where ticket should win more) isn't here.

## Cross-host atomic fetch_add works with explicit flush

`tests/cxl_atomic_xhost` (new): 2 hosts × 4 fork × 10 000 iterations on a
shared CXL counter, with `flush_line; full_fence; fetch_add; flush_line;
store_fence;` wrapping every op.

Result: `FINAL counter=80000 expected=80000 OK`. So cross-host atomic
increment on the PCIe-switched memory server is achievable as long as
the local cache is flushed before and after.

## What breaks — YCSB runner hangs with ticket lock

`cxl_ycsb_runner_C` at T=1 (2 hosts × 1 client) hangs indefinitely with
`FUSEE_USE_TICKET_LOCK=1`. Both processes pin 99 % CPU. No output past
the `trans_go` trace. Revert to LFM — works fine.

The atomic microbench passes, so the atomic itself is OK. The ticket
lock in isolation (the decomp bench at N=1..4) also works. Something
specific to the YCSB runner's usage pattern trips it — possibly:

- Multiple locks (1 per bucket × 65k buckets) initialized concurrently
  by host 0 *after* host 1 has already begun fetch_add'ing. Non-coherent
  CXL could mean host 1 sees init state (0,0) while host 0 is still
  memset'ing → host 1 claims ticket 0, enters crit before host 0 is
  finished with `attach(init_region=true)`.
- Or: `ticket_mutex_init` itself isn't fully visible to the other host
  because the final `CACHELINE_STORE(&magic, ...)` flushes only that
  one line, not `next_ticket` and `now_serving`. A concurrent fetch_add
  from host 1 could see uninitialized values cached locally.

Needs more instrumentation to confirm. Not pursued further today.

## Disposition

Keep the implementation in-tree under the opt-in flag. Default build
continues to use LFM so the baseline scaling sweep's numbers aren't
regressed. Phase 2 re-attempted after Phase 4 (per-client ring), when
at least:

1. The per-host init race can be harder to hit (per-client rings mean
   fewer total lock acquire attempts overlap with init).
2. Or: move bucket lock init to a per-client explicit barrier rather
   than implicitly rely on host 0's memset reaching the other host in
   time.

## Why Phase 4 goes next instead

The write-tail problem that Phase 2 was supposed to fix turned out to
be scheduler-dominated (p99 45 ms = Linux scheduler tail under 172
contenders, not lock algorithmic). Phase 3 (SCHED_FIFO + isolcpus) is
the more direct fix for scheduler tail but is shelved. Phase 4 fixes
the structural cap on A/B write scaling, which is a bigger throughput
lever.
