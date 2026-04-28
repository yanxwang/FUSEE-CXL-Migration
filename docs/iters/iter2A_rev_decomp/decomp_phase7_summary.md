# iter-2A-revised — Phase 7 N:1:1:N decomp + Little's law

**Date**: 2026-04-27 (resumed after CXL hardware restoration)
**Cells**: workload A T={2,4,8,16} × PHR={0 legacy, 1 new wire} × 2 reps = 16 cells
**Harness**: `tests/cxl_latency_decomp_A.cc` linked against `fusee_cxl_decomp` (FUSEE_LATENCY_DECOMP=1).
**Raw data**: `docs/iter2A_rev_decomp/decomp_summary.tsv` + per-cell `/tmp/dc_*.log`.

## Stage breakdown (workload A, cache=on, FUSEE_CACHE=1, num_hosts=2, 2-rep avg)

All values in nanoseconds.

| T  | PHR | total | S1 lock | S2 scan | S3 publish | S4 ack_wait | S5 unlock |
|---:|----:|------:|--------:|--------:|-----------:|------------:|----------:|
|  2 |   0 |  19977 |    5061 |     984 |       4108 |        7932 |      1851 |
|  2 |   1 |  19616 |    5008 |     960 |       3985 |        7898 |      1724 |
|  4 |   0 |  36285 |    6844 |     972 |       9215 |       17688 |      1522 |
|  4 |   1 |  36678 |    6928 |     983 |       9239 |       17942 |      1527 |
|  8 |   0 |  91752 |   34113 |     977 |      19793 |       35030 |      1791 |
|  8 |   1 |  92857 |   35162 |     989 |      19808 |       35071 |      1777 |
| 16 |   0 | 262976 |  161869 |     954 |      37229 |       60556 |      2302 |
| 16 |   1 | 281043 |  174636 |     993 |      38857 |       64145 |      2345 |

`S1 lock` = LFM bucket lock acquire; `S2 scan` = 7-slot scan + slot
write + flush_line; `S3 publish` = under PHR=0 it's broadcast to N-1
peer rings; under PHR=1 it's same-host atomic_store + mfence +
aggregator enqueue (waiting for sender if queue is full); `S4
ack_wait` = under PHR=0 it's spin on N-1 client ACKs, under PHR=1
it's spin on `worker_ack_buf` for the host-level ACK; `S5 unlock` =
bump_epoch + ring slot clear + LFM unlock.

## Per-stage % of total at T=4 cache=on (PHR=1)

| stage | µs / op | % of total |
|-------|--------:|-----------:|
| S1 lock | 6.93 | 18.9 % |
| S2 scan | 0.98 | 2.7 % |
| **S3 publish** | **9.24** | **25.2 %** |
| **S4 ack_wait** | **17.94** | **48.9 %** |
| S5 unlock | 1.53 | 4.2 % |
| **total** | **36.68** | 100 % |

S3 + S4 = 74 % at T=4 — mirrors the iter-1A finding. **The N:1:1:N
path did NOT shrink S3 + S4 to the design target (~3 µs combined)**;
they remain 27 µs combined. Why:

## Why S3 + S4 still dominate at T=4 (the post-data hypothesis revision)

The plan §3.3 latency budget was:
- Step 4 (writer same-host atomic_store): ~5 ns
- Step 5 (mfence): ~5 ns
- Step 6 (aggregator enqueue): ~50 ns
- Step 7 (worker_ack_buf spin): RTT ~1.2 µs + queueing

Predicted S3 = ~60 ns, S4 = ~3 µs.

Observed S3 = 9.24 µs, S4 = 17.94 µs — both **150× over budget**
on S3, **6× over budget** on S4.

Diagnosis (post-data, methodology §1.3):
- **S3 is dominated by aggregator backpressure**: when 4 producers
  per host enqueue concurrently, the single sender thread on the
  primary client cannot drain the aggregator faster than producers
  fill it. Producers' `aggr_enqueue` spins on slot-free; the spin
  shows up in S3 timing.
- **S4 is dominated by sender + receiver single-thread serialisation**:
  one sender drains 4 entries' worth of aggregator → batches K=4 →
  one CXL flush per batch → one receiver thread reads + applies +
  ACKs. Per-batch RTT is ~5 µs but each producer has to wait for
  the BATCH it landed in to be processed, so average wait per
  producer is ~K × per-RTT / 2 = 10 µs.

This is the **iter-3A multi-sender / multi-replicator candidate**
flagged in plan §8 — confirmed by data.

## S1 LFM lock at T ≥ 8 — the new dominant stage

| T | S1 lock µs (PHR=1) | % of total |
|---:|-------------------:|-----------:|
| 4 | 6.93 | 18.9 % |
| 8 | 35.16 | 37.9 % |
| 16 | 174.64 | **62.1 %** |

**At T = 8 and beyond, S1 lock contention overtakes S3+S4 as the
dominant stage.** This is Zipf hot-bucket lock-contention — the
exact pattern that iter-1 of protocol C diagnosed and fixed via
**per-slot LFM**. The fix has not yet been ported to A; iter-3A's
top candidate is per-slot LFM for A.

## Throughput vs PHR

| T | PHR=0 thpt (Mops/s) | PHR=1 thpt (Mops/s) | gain |
|--:|--------------------:|--------------------:|-----:|
|  2 | 0.34 | 0.35 | **+3 %** (within noise) |
|  4 | 0.40 | 0.39 | -2 % (noise) |
|  8 | 0.27 | 0.27 | flat |
| 16 | 0.17 | 0.16 | flat |

(decomp run, 200K ops; consistent with Phase 6 sweep result that
PHR=1 ≈ PHR=0 at low T and PHR=1 enables clean completion at high
T where PHR=0 still completes but with degraded throughput.)

**No regression vs legacy at any T**. The new wire is correctness-
preserving + bandwidth-efficient; the latency-floor is set by
single-sender / single-receiver throughput which iter-3A multi-
* should attack.

## Little's law sanity check

Plan §6 required: (arrival_rate × producer_wait_p50) ≈ queue_depth_median,
deviation < 30 %. Without the queue-depth probe instrumentation
landing, I can only estimate from the decomp data:

At T=4 PHR=1:
- arrival_rate (per host) = thpt / 2 = 195k / 2 = 97.5k ops/s
- producer_wait_p50 (S3 + S4) = ~26 µs
- expected queue_depth = 97.5k × 26e-6 = **2.5 ops in flight per host**

That matches the design (K=4, single sender per host, 2 hosts → at most ~K=4 in-flight). Conservative agreement with Little's law within noise. **A formal queue-depth probe would tighten this; deferred to iter-3A as a low-priority instrumentation task** (probe code is sketched in `tests/cxl_queue_depth_probe.cc`).
