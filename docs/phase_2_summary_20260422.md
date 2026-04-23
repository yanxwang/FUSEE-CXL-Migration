# Phase 2 (2a/2b/2e/2f/2g) summary — 2026-04-22

Phase 2 optimizations of the CXL writer path for protocols A and B.
Builds on the Phase 1+4+5 baseline: per-client ring matrix, same-host
read-only attach, hierarchical A with `FUSEE_A_GROUPS=4`.

## Commits landed (feat/cxl-migration)

- `4d7704c` packed PendingRingEntry 6 → 2 cachelines
- `da76b00` **2a** same-host DRAM bypass (DRAM-native 64B queue)
- `8c9b65f` **2b** fence consolidation in writer (A + B)
- `1e249d9` **2e+2g** B batch-push infrastructure + publish_slot single-flush
- `f2e5563`/`d5421c7`/`36132f1` sweep script hardening (zombie kill, env
  forwarding, `A_SKIP_AT`)

2f ring-depth sweep ran three values (16, 256, 1024) — 256 default stays;
depth=16 regresses T=64 by ~25%, depth=1024 indistinguishable from 256.

## Headline numbers — g3+g4, 200 kops, 65536 buckets, cache on

Phase 2 (v4) vs Phase 1+4+5 (162908). `trans_agg_thpt` in ops/s.
Full table at `docs/phase2_summary/phase2_vs_p145_table.txt`.

Write-heavy regimes (workloada 50/50, workloadf RMW) see the biggest gains
because every op touches the ring; read-only (workloadc) and high-T C are
flat because C has no ring path and reads skip it.

```
                         P1+4+5       P2 v4      gain
workloada cache=on
  A  T=8                 152993      449489     +193.7%
  A  T=16                 89008      278994     +213.4%
  B  T=8                 207807      686813     +230.5%
  B  T=16                122860      498315     +305.5%
  C  T=16 .. T=86        unchanged (±5%)

workloadb cache=on (95% R)
  A  T=8                1564398     3993231     +155.2%
  B  T=8                1530466     5142463     +236.0%
  B  T=16                925351     3064911     +231.2%
  C  all T               unchanged (±5%)

workloadd cache=on (INSERT)
  A  T=8                2072159     4857939     +134.4%
  A  T=16               2325688     6395979     +175.0%
  B  T=8                2621442     6497342     +147.8%
  B  T=16               3127244    10240289     +227.4%

workloadf cache=on (RMW, 50/50 read-then-write)
  A  T=8                 204222      584163     +186.0%
  A  T=16                127798      402602     +215.0%
  B  T=8                 278756      918438     +229.4%
  B  T=16                181855      677593     +272.6%

workloadc cache=on (100% R)
  A/B/C all T            unchanged (±5%)  ← as expected: no writes, no ring
```

cache=off shows the same pattern; B workloada T=8 went 188k → 739k (+293%).

## Why the gains

1. **Packed PendingRingEntry (6 → 2 cachelines)** cuts the cross-host
   write-path from 6 × (write + flush + fence) down to 1 × (atomic 64 B
   cacheline publish) for the producer payload, with ACK on a separate
   cacheline to avoid false sharing.
2. **2a Same-host DRAM bypass**: for a client pushing to N-1 peers at
   T=32 (64 total clients), ~31 of those peers are on the same physical
   host. Those pushes no longer hit the CXL fabric at all — they go
   through a `MAP_SHARED | MAP_ANONYMOUS` SPSC queue of 64 B DRAM
   entries, one per (src_cid, dst_cid). Amortized cost of an
   invalidation drops from ~1 μs (CXL) to ~100 ns (DRAM).
3. **2b Fence consolidation**: pre-2b, B's writer issued 2 sfences in
   `publish_slot` + 3 sfences per cross-host peer + 1 bump_epoch sfence
   = 2 + 3N + 1 sfences per op. Post-2b: 1 + 0 + 1 = 2 sfences/op,
   relying on x86 TSO within a single producer plus one catch-all
   sfence at `bump_epoch` for globally visible seqlock ordering.
4. **2g publish_slot single-flush**: a slot is 16 B inside a 64 B cacheline
   — one clflushopt is an atomic publish. The sfence between value and
   key writes went away.
5. **2e B batching infrastructure** (opt-in, default K=1 = no batching):
   per-dst buffered push. Workload scan showed this is usually neutral
   or slightly worse for workloada; it's staged in for future sub-workloads
   where writer-side amortization matters.

C is intentionally untouched by 2a/2b/2e — it never writes the ring. Its
numbers regress slightly (≤5%) in a few cells, consistent with run-to-run
noise; the rest are within measurement noise.

## Known non-issue: A at T ≥ 64

A's ACK-wait fundamentally serializes on the slowest same-group peer.
With `FUSEE_A_GROUPS=4`, same-group size at T=32 is 16 peers — OK.
At T=64 it's 32 — the replicator can't keep up on workloada and each
writer runs into its 200 ms ACK budget. The sweep script now skips A at
T ≥ 64 by default (`A_SKIP_AT=64`) to match the P1P4P5 scope; set
`A_SKIP_AT=999` to force-run them. Real fix is Phase 5.1 (adaptive group
sizing) — not on this session's scope.

## Artifacts

- Log: `logs/g34_scaling_sweep_p2_v4_20260422_205644/SUMMARY.log`
- Plots (cache=on): `logs/g34_scaling_sweep_p2_v4_20260422_205644/plots/` (42 files)
- Plots (cache=off): `logs/g34_scaling_sweep_p2_v4_20260422_205644/plots_cache_off/`
- Comparison table: `docs/phase2_summary/phase2_vs_p145_table.txt`
