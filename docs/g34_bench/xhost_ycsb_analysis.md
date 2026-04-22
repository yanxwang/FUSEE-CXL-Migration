# g3 + g4 real cross-host YCSB: A vs B vs C (kernel 6.15.0)

**Setup**: 2 machines (g3, g4), each running 1 process on its own
`/dev/dax0.0` mmap, sharing the same 512 GiB CXL memory server via the
PCIe switch fabric. Role-mode `cxl_ycsb_runner_{A,B,C}`, workloads A
and C from the official YCSB traces, 65 536 buckets, 200 k trans ops
capped per workload. 12 runs total; log:
`docs/fusee_ycsb_g34_xhost_20260422.log`, plot:
`docs/fusee_ycsb_g34_xhost_20260422.png`.

## Agg trans throughput (kops/s)

| workload | opt | cache=off | cache=on | on/off |
|----------|-----|----------:|---------:|-------:|
| a (50% R / 50% U) | A | 180 | 192 | 1.07× |
| a | B | 229 | 259 | 1.13× |
| a | C | 329 | 342 | 1.04× |
| c (100% R) | A | 713 | 3 650 | **5.12×** |
| c | B | 740 | 3 603 | **4.87×** |
| c | C | 709 | 1 792 | 2.53× |

## What the numbers say

**writes (workload a — 50 % updates mixed with 50 % reads)**:
`C (342) > B (259) > A (192)` cache-on, identical ordering cache-off.
C wins because its writer does `lock + store + epoch_bump + unlock`
with no peer ACK; B adds a per-peer ring push (non-blocking); A
further waits for every peer to ACK the push before releasing.

**pure reads (workload c)**:
With cache off, all three tied at ~710-740 k ops/s (no lock on the
read path → protocol doesn't matter, you're reading two 64 B lines
from CXL per lookup).

With cache on, `A ≈ B (3.6 M) >> C (1.8 M)`. A and B serve reads
entirely from DRAM on a cache hit; C has to re-load the bucket's
`write_epoch` cacheline from CXL to check freshness on every read.
The 3 μs CXL round trip × ~1 M ops/s = ~3s of CXL bus time per host,
which caps C at half of A/B.

## Cross-check against single-machine numbers

- emr (single-host private CXL, 4 procs, pure reads): ~1.2 M ops/s
- g3 alone (single host, 4 procs, pure reads): ~1.2 M ops/s
- g3+g4 cross-host (2 hosts, 1 proc each, pure reads, workload c,
  cache off): 713 k ops/s — *lower* than single host.

That's expected: with only 2 procs on the memory server, we can't
saturate the shared fabric. The 4-proc single-machine case runs all
4 procs on the same CPU's memory controller, hitting memory-bus-level
concurrency. Cross-host adds a PCIe-switch hop for every access and
splits the total bandwidth between g3 and g4.

To see real scaling benefits of cross-host, we'd need more threads
per host (task 4's `cxl_kv_bench_threads` with 16 threads per host
would give 32 concurrent clients). That's the natural follow-up.

## Why this worked today but not before

Two bugs blocked this run during the overnight window:

1. **Fork-mode stale mmap** (fixed in task 2): the runner didn't
   fork, but the same class of bug lurked in role-mode — non-primary
   host read a stale `init_done==1` from a prior run's bytes that
   devdax mmap left there. Fixed by adding a `FUSEE_RUN_COOKIE` that
   the orchestrator generates fresh per run; non-primary waits for
   the cookie to match, not just for init_done to be 1.

2. **`MAX_HOST_NUM` skew between hosts**: for task 4 I bumped
   `MAX_HOST_NUM` 8→32 on local, rsync'd to g4, but forgot g3.
   Each `BucketLockEntry` grew from ~1.5 KB to ~6 KB, so
   `CxlKvStore::bytes_for(65536)` gave two different sizes on the two
   hosts. `cxl_region_init` then rounded to 2 MiB and the `shared`
   struct ended up at **different physical offsets** on each host —
   each host was reading and writing flags that the *other* host
   wasn't looking at. Fixed by rsyncing + rebuilding.

Takeaway for the final `docs/g34_bench/g34_kernel_new_observations.md`:
any compile-time constant affecting CXL region layout must be
identical across all hosts sharing the region. A build-time self-check
(primary writes its `sizeof(YcsbShared) + bytes_for(num_buckets)` at
the start of the region; non-primary verifies a match) would have
caught this immediately.
