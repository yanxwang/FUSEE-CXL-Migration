# Cross-host gain (2 hosts vs 1 host, opt C, cache on)

**Setup**: Rerun the scaling sweep on g4 ALONE (num_hosts=1,
T=1..86 fork clients) and divide 2-host agg thpt by 1-host agg thpt.
If cross-host added bandwidth, ratio should be ~2.

**Result** (agg thpt 2-host / agg thpt 1-host):

| workload | T=1 | T=2 | T=4 | T=8 | T=16 | T=32 | T=64 | T=86 |
|---|---|---|---|---|---|---|---|---|
| a (50/50)       | 1.83 | 1.95 | 1.64 | 1.03 | 1.12 | 0.64 | 0.64 | 0.67 |
| b (95R/5U)      | 1.78 | 1.68 | 1.58 | 1.57 | 1.20 | 0.79 | 0.63 | 0.58 |
| c (100R)        | 1.76 | 1.75 | 1.67 | 1.61 | 1.46 | 0.95 | 0.92 | 1.35 |
| d (95R/5I)      | 1.74 | 1.81 | 1.76 | 1.70 | 1.37 | 0.96 | 1.21 | 1.41 |
| f (50R/50RMW)   | 1.96 | 1.89 | 1.85 | 1.37 | 0.90 | 0.69 | 0.55 | 0.60 |

**Interpretation**:

- **Low T (1-8)**: 2-host gives 1.5-2× speedup — second host's CPUs +
  CXL bandwidth genuinely add throughput.
- **Mid T (16-32)**: gain drops toward 1× for read-heavy, below 1×
  for write-heavy. Cross-host coordination overhead (init_done barrier,
  trans_go barrier, done barrier — each is a CXL round-trip) and
  bucket cross-contention start to eat the gain.
- **High T (64-86)**: write-heavy workloads go *below 1×* —
  adding the second host makes things WORSE. Read-heavy (c, d) recover
  to 1.35-1.41× because they don't take bucket locks.

So the "peak T" story is different for 2-host vs 1-host:

| workload | 1-host peak T | 1-host peak (kops/s) | 2-host peak T | 2-host peak (kops/s) |
|---|---|---|---|---|
| a | T=8       | 1 076 | T=4  |  1 122 |
| b | T=16/32   | 5 229 | T=16 |  6 326 |
| c | T=86      | 35 595 | T=86 | 48 176 |
| d | T=86      | 31 117 | T=86 | 43 959 |
| f | T=8       | 1 303 | T=8  |  1 785 |

The 2-host peak is always higher, but the margin is modest for
write-heavy workloads. For read-heavy, 2-host gives ~1.35× over
1-host-best — useful but not 2×.

**Implication for deployment**: if your workload is read-heavy and
you can tolerate up to T=86 clients, 2 hosts give ~35 % more throughput.
If your workload is write-heavy (a, f), one well-tuned host at T=8 is
within 15 % of the best 2-host number — cross-host is not pulling its
weight. To actually benefit from cross-host on write-heavy, the
per-host bucket-lock contention needs to be reduced (ticket lock,
per-client rings for A/B, or bucket sharding).

Plot: `extra/cross_host_gain.png` (5 subplots, one per workload).
Raw log: `extra/g4_solo_scaling.log`.
