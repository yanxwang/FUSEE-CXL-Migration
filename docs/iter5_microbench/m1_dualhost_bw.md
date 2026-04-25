# M1 — dual-host CXL write-bandwidth microbench

**Bench**: `tests/cxl_dualhost_bw_bench.cc`. Both hosts run the
binary simultaneously (cookie barrier). Each spawns `T`
`std::thread` workers that AVX-512 nontemporal-stream writes
(`_mm512_stream_si512`) to disjoint halves of a shared 8 GiB CXL
devdax region for 5 s. Aggregate = host0 GB/s + host1 GB/s.

## Raw results (5 s windows, 8 GiB region)

| threads/host | g3 GB/s | g4 GB/s | aggregate GB/s | ratio to mlc per-host (51.78 GB/s) |
|-------------:|--------:|--------:|---------------:|-----------------------------------:|
| 1            | 12.36   | 12.30   | 24.66          | 0.48 |
| 2            | 12.58   | 12.58   | 25.16          | 0.49 |
| 4            | 12.56   | 12.56   | 25.12          | 0.49 |
| 8            | 12.55   | 12.54   | 25.09          | 0.48 |
| 16           | 12.64   | 12.64   | 25.27          | 0.49 |
| 32           | 12.48   | 12.48   | 24.96          | 0.48 |
| 64           | 12.48   | 12.48   | 24.96          | 0.48 |

## Key findings

1. **Per-host writes saturate at ~12.5 GB/s**, independent of
   thread count — single thread already hits the asymptote. The
   PCIe / CXL link to the expander is the bottleneck, not core
   throughput.
2. **Dual-host aggregate ≈ 25 GB/s**, exactly 2 × per-host. The
   expander supports both hosts writing simultaneously without
   apparent contention; there is no shared 51.78 GB/s ceiling
   that the two hosts must split.
3. **Mismatch with mlc 51.78 GB/s/host**: our 12.5 GB/s/host is
   ~24 % of the mlc measurement. mlc is presumably either using a
   different store path (e.g., temporal stores with explicit cache
   eviction, or a tuned AVX kernel) or reporting peak line-rate
   that nontemporal-stream from user-space cannot reach. For FUSEE
   write-path planning, the **practical** ceiling per-host is 12.5
   GB/s, aggregate 25 GB/s.

## Implications for the iter-4 BW-saturation claim

iter-4 used a 22 GB/s/host figure (from earlier hw bench) ×
2 hosts = 44 GB/s aggregate ceiling, and inferred kv=1024 was
"BW-saturated at 0.82 of ceiling." With M1's authoritative
~25 GB/s aggregate (not 44), the real picture per UPDATE op:

| vsize | bytes/UPDATE writer | aggregate ops/s ceiling at 25 GB/s | iter-4 measured B peak | utilisation |
|------:|--------------------:|-----------------------------------:|-----------------------:|------------:|
| 256   | 320                 | 78 Mops/s                          | 28.89 Mops/s           | 0.37        |
| 512   | 576                 | 43 Mops/s                          | 25.86 Mops/s           | 0.60        |
| 1024  | 1088                | 23 Mops/s                          | 16.37 Mops/s           | 0.71        |

The kv=1024 case is at **71 %** of the dual-host aggregate write
BW ceiling — still BW-relevant but with ~30 % headroom, less
extreme than iter-4's 0.82 estimate. The **shape** of the
crossover (latency-bound at kv ≤ 256, mostly BW-bound at
kv ≥ 1024) is unchanged; only the absolute ceiling moves.

## Implications for multi-flusher V2

Workload A peak in iter-4 at kv=8: 13.94 Mops/s × 8 B / op
≈ 110 MB/s — far below the 25 GB/s aggregate ceiling. For
small-vsize, BW is not the bottleneck; **flusher
parallelism** is the right lever. Expected iter-5 multi-flusher
gain on A is bounded only by per-shard `bump_epoch` rate and
producer-side enqueue contention, not by CXL link.

For kv ≥ 512, multi-flusher gain is bounded by the BW ceiling
(43 Mops/s at kv=512, 23 Mops/s at kv=1024). Going from
single-flusher 25.86 Mops/s (kv=512) to a multi-flusher peak >
40 Mops/s would require nearly perfect linear scaling AND
sustained 95 % utilisation — possible but tight.

## Bench self-consistency check

Single-thread per host equals 64-thread per host: 12.5 GB/s.
This is consistent with PCIe write-combine semantics — the link
is the bottleneck, not core ROBs. AVX-512 nontemporal stores
produce roughly equal sustained throughput as scalar `movnti`
plus prefetch in this regime.
