# g3+g4 scaling sweep analysis (2026-04-22)

**Setup**: 2 hosts (g3, g4) × T clients/host × 3 protocols (A/B/C) × 5
workloads (a, b, c, d, f — e skipped; scan unimplemented) × 2 cache modes
= **240 runs**, all clean. Each run's trans phase is 200 k ops split across
2T client processes, each process a fork child with its own CxlKvStore +
unique LFM slot (global_id = host * T + client_id). Raw log:
`SUMMARY.log`.

Load factor check: 2 × 86 = 172 clients × ~1160 trans ops each = 200 k,
spread over 65 536 buckets, ~3 ops/bucket → no inserts during trans.

## Clamp note for A and B

Options A and B were **clamped to 1 worker per host** regardless of the
requested T. This is because:

- A's per-op path enqueues a replication entry into `rings[src][dst]` and
  spins on the peer's ACK. The ring tail/head pointers are per-host pair,
  not per-thread. Multiple intra-host workers pushing to the same ring
  would race on tail.
- B's pending-ring push is similarly per-host.
- Each `CxlKvStore{A,B}` instance spawns its own replicator thread. Two
  per-host processes would each try to consume the same peer's ring and
  corrupt the head cursor.

Implementing "per-client" rings is a separate, larger refactor (see
`docs/option_a_side_track.md`). The clamp means **A and B curves across
T are essentially flat** in the plots — the 2 hosts × 1 process
aggregate is the same regardless of T. That is the honest answer under
the current FUSEE-CXL architecture, not a bug.

**C has no such state**: it's a lock-epoch protocol with no replicator,
so C clients are independent and each gets its own LFM slot via
`global_id`. C is the one that actually scales with T.

## Peak throughput per (opt, workload), cache on

From `extra/summary_table.md`. Values in **kops/s agg** across both hosts.

| workload           | A peak        | B peak        | **C peak**       |
|--------------------|---------------|---------------|------------------|
| a (50 % R / 50 % U) | 188 @ T=64   | 238 @ T=86   | **1 122 @ T=4**  |
| b (95 % R / 5 % U) | 1 157 @ T=1  | 1 428 @ T=8  | **6 326 @ T=16** |
| c (100 % R)        | 3 318 @ T=86 | 3 402 @ T=32 | **48 176 @ T=86** |
| d (95 % R / 5 % I, latest) | 1 214 @ T=4 | 1 493 @ T=1 | **43 959 @ T=86** |
| f (50 % R / 50 % RMW) | 262 @ T=8   | 362 @ T=2    | **1 785 @ T=8**  |

## Stories the data tells

### 1. Read-dominated workloads: C wins BIG at high concurrency

Workload c (100 % reads) and workload d (95 % reads):

- A / B: ~3.3 M ops/s AGG, essentially flat across T (clamped at 1
  worker per host; 2 workers total saturate their trivial read path).
- **C: 48 M ops/s at T=86** on workload c, **44 M on workload d** —
  14× higher than A/B at the same T.

Why? C's read path takes no lock and does no replication. 172 parallel
read-only clients across 65 536 buckets hit almost no collisions and
scale nearly linearly. C's scaling efficiency at T=86 is
`thpt(86) / (86 × thpt(1))` ≈ 33 % for workload c (plot
`extra/scaling_efficiency_C.png`), which is low but expected given:

- Each host has 86 CPUs; a 172-client deployment oversubscribes the
  machine's memory controller even for pure-read traffic.
- CXL load latency (~3 μs/read) is a hard bandwidth floor per read.

### 2. Write-heavy workloads: C scales sub-linearly, eventually degrades

Workload a (50/50), workload f (50 R / 50 RMW):

- Peaks at **T=4 / T=8** (1.1 M / 1.8 M), then drops progressively. By
  T=64 they are worse than T=1.
- Mechanism: LFM bucket lock contention. With 172 workers contending
  across 65 k buckets, at 50 % writes each bucket gets touched by many
  workers simultaneously. LFM's Lamport fast-mutex has O(N) worst-case
  retry + backoff (we added the backoff earlier in the week). At high
  N this dominates.

### 3. Light-write workloads: a moderate peak, then degrade

Workload b (95 R / 5 U), workload d (with C):

- C on b peaks at T=16 (6.3 M), degrades by T=86 back to T=4 level.
- C on d peaks at T=86 (44 M) — *no degradation*.
- Difference between b and d: b is UPDATEs (existing keys), d is INSERTs
  ("latest" distribution, recent keys). Latest-distribution inserts
  target a narrow bucket range → contention concentrates → degrades
  *faster* than uniform inserts? Actually the opposite here; d
  aggregates writes at recent keys but ycsb-runner's distinct thread
  slices avoid cross-thread hotspot. Needs more investigation.

### 4. Cache on vs off

For read-dominated workloads (b, c, d), cache-on speeds things up
1.5-4× on A/B (see `extra/cache_speedup_{A,B}.png`). For C, cache-on
gives only ~1.3× because C still has to touch one CXL cacheline per
read to check the epoch. For write-heavy workloads (a, f), cache has
little effect (updates invalidate cache on every write).

## Latency picture (write p99)

`A_lat_workloada_write.png` etc.: at T=1, p99 is ~20 μs for A, ~16 μs
for B, ~10 μs for C. At C's T=86, p99 blows up to 1-10 ms due to LFM
contention tails.

## What the 30-plot deck shows

Under `docs/g34_scaling_ycsb/`:
- `A_thpt_{a,b,c,d,f}.png`, `B_thpt_...`, `C_thpt_...` — 15 throughput
- `A_lat_..._write.png`, `A_lat_..._read.png` (where applicable) — 27 latency
- Plus 42 more under `cache_off/` for cache-off variants.

Under `docs/g34_scaling_ycsb/extra/`:
- `abc_compare_<wl>.png` — 5 thpt comparison plots
- `abc_compare_<wl>_lat.png` — 4 write-p99 comparison plots
- `cache_speedup_{A,B,C}.png` — cache-on/off ratio per workload
- `scaling_efficiency_C.png` — C speedup vs ideal linear
- `summary_table.md` — the peak-T table above

## Recommended follow-up experiments

1. **Bigger bucket count** for opt C: 65 k → 1 M buckets. At T=86 × 2
   hosts = 172 clients × 200k/172 ≈ 1 k ops/client, bucket load factor
   is already 3. More buckets should reduce collisions and let C's
   scaling on write-heavy workloads hold longer.
2. **Per-client ring for A/B** so they genuinely scale with T.
   Approximate effort: ~500 LOC in `cxl_kv_ops_{A,B}.cc` + a matching
   pending-ring per-host-pair × per-client matrix.
3. **Bucket lock alternatives**: replace LFM with a ticket lock or MCS
   queue lock. LFM's worst-case backoff dominates the T=64/86 write
   tail. Our latency decomposition earlier pointed at this; the new
   scaling data confirms it's the single biggest limiter for
   write-heavy workloads.
4. **Workload e (scan)**: implement scan, re-run the matrix, see how
   scan-heavy workloads (90%+) scale with C's lock-free reads.
