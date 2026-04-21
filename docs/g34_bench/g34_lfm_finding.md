# LFM 4-proc wedge on g3/g4 PCIe-switched CXL (finding, not fix)

**Date**: 2026-04-21 ~03:00 CDT
**Testbed**: g3 / g4 (Haidilao env) with shared CXL memory server accessed
via PCIe switch; each host's `/dev/dax0.0` maps the same 256 GB region.

## What works

- Cross-host magic-word handshake (`cxl_xhost_test`): g3 writes 12 cachelines
  of pattern + handshake, g4 reads back and verifies. Round-trip ~1.5 s
  (dominated by the 200 μs spin cadence in the waiter). No corruption.
- Single-host primitives on g4 `/dev/dax0.0`:
  - `cxl_mm_test`: open + ftruncate + write + read: 4 ms total.
  - `cxl_mm_mp_test` (fork 2-proc handshake): passes.
  - `cxl_bucket_lock_test` (fork 2-proc, 500 iters, 1 bucket under LFM):
    passes. 17.3 μs per critical section.
- Single-host multi-proc bench on g4 at `num_hosts=2` (fork mode, Opt C,
  500 ops/host, 8192 buckets, wr=0.5): finished in 4 ms, agg 277 k ops/s.

## What wedges

- Single-host fork-mode bench on g4 at `num_hosts=4` (same binary, same
  parameters as the 2-host run, just `-DCONSENSUS_OPT=C` at 4 procs):
  **hangs indefinitely** at 99 % CPU on every child. `timeout 30` does
  not terminate them cleanly. `pkill -9 cxl_kv_bench` clears the
  processes but subsequent ssh to the host starts failing
  (`Connection closed by 192.168.128.74 port 22`) — suggests the kernel
  is also unhappy. Host had to be rebooted.

## Hypothesis

LFM (Lamport's Fast Mutex, `cxl_shm_profiling/locks/lfm_lock.c`) has an
unbounded retry loop:

```c
retry:
  b[id] = 1; x = id+1;
  if (y != 0) { b[id] = 0; wait y == 0; goto retry; }
  y = id+1;
  if (x != id+1) { ...wait for all b[j]...; if (y != id+1) retry; }
```

It is provably correct assuming writes *eventually* become visible. On
emr (direct-attach CXL, same cache domain as the CPU), writes propagate
within a cacheline-flush cycle and LFM finishes at the expected
low-contention latency even at `num_hosts=4`. On g3/g4 the CXL memory
lives behind a PCIe switch on a separate memory server; the fabric's
write-visibility window appears to be long enough that at 4 concurrent
proposers the fast path *never* succeeds — `x` keeps getting overwritten
before the current proposer's `y` read completes.

## Why 2 procs works but 4 does not

Only one other contender at `num_hosts=2`; the window of "writes not yet
visible" rarely contains a contending write. At `num_hosts=4`, three
contenders are writing `x` concurrently; each proposer's `CACHELINE_LOAD(&x)
== id+1` check almost always fails, sending it to the slow path which
spins on `b[j]` for every peer and on `y`. With slow fabric, this slow
path is also never observed to clear.

## Practical workaround for the 9 AM goal

Run the g3+g4 cross-host A/B/C × YCSB A+C sweep with **one process per
slave** (`FUSEE_NUM_HOSTS=2`), i.e. total concurrent contenders = 2. This
stays inside the fast-path window. Bucket throughput will be lower per
host than on emr (because each host's own `/dev/dax0.0` access is slower
when the memory lives over the switch), but the three protocols' relative
ordering should still be meaningful.

## Items NOT done tonight

- Verify whether `cxl_shm_profiling/bench/ycsb_abc_bench.c` (the mini-bench)
  also wedges at 4-proc on g3/g4. If it does, the fault is in LFM, not in
  our port. If it does not, the fault is in our bench.
- Modify `shm_mutex_lock` to add a bounded retry + sleep backoff. Risks
  breaking emr behavior; would need side-by-side regression.
- Investigate whether `daxctl` or the kernel module has a knob to force
  stronger write ordering on PCIe-switched CXL.
