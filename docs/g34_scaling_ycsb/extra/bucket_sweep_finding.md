# Bucket-count sweep at T=86 — bucket count is NOT the bottleneck

**Hypothesis tested**: C on workload-a/f at T=86 degrades because 65 536
buckets aren't enough for 172 concurrent write-heavy clients.

**Test**: T=86 fixed, `num_buckets ∈ {16 k, 64 k, 256 k, 1 M, 4 M}`,
workloads a (50/50) and f (50 R / 50 RMW), cache on.

| workload | buckets | agg thpt (kops/s) | w_p50 (μs) | w_p99 (μs) |
|---|---|---|---|---|
| a | 16 k    | 228 |  9.4 | 45 301 |
| a | 64 k    | 232 | 10.5 | 45 223 |
| a | 256 k   | 224 | 11.5 | 45 289 |
| a | 1 M     | 228 | 11.9 | 35 807 |
| a | 4 M     | 234 | 12.0 | 24 099 |
| f | 16 k    | 329 |  8.9 | 44 939 |
| f | 64 k    | 379 |  9.5 | 44 905 |
| f | 256 k   | 333 | 11.1 | 33 070 |
| f | 1 M     | 357 | 11.2 | 44 597 |
| f | 4 M     | 340 | 12.0 | 34 813 |

**Conclusion**: thpt is **flat across 256× range in bucket count**. Bucket
contention is not the limit.

**What IS the limit**: look at the latencies. w_p50 is ~10 μs at every
bucket count — that's healthy C write latency. But w_p99 is 24-45 ms.
That 4-order-of-magnitude p50→p99 gap points at **scheduler tail**, not
bucket contention. At 172 client processes × 2 hosts and 86 cores per
host, each process ≈ 1 core, but any preemption (context switch, IRQ,
page fault) stops that client for 10+ ms, and every other client
waiting on that client's bucket stalls.

This suggests the next productive follow-ups are:
1. `chrt -f 10` or similar to give client processes SCHED_FIFO, reduce
   preemption tail.
2. Replace LFM's spin-then-nanosleep with futex wake/wait once a
   contender sits on a bucket for > some threshold — lets the kernel
   park waiters cleanly instead of burning CPU + leaking cacheline.
3. Pin client processes to specific cores so scheduler doesn't
   migrate them away from their CXL NUMA node.

Priority still: P3 from `followup_experiments.md` (ticket lock) addresses
the same core issue from a different angle.
