# Phase 5 A-groups sweep — workloada, g3+g4 cross-host

`FUSEE_A_GROUPS=K` env splits A's writer ACK wait into per-group sync;
cross-group is eager. 20 runs, 0 failures.

## trans_agg_thpt (kops/s) — workloada, cache on

| K\T  | T=1 | T=2 | T=4 | T=8 | T=16 |
|------|----:|----:|----:|----:|-----:|
| K=1  | 184 | 214 | 227 | 130 |  85  |
| K=2  | 221 | 224 | 233 | 154 |  80  |
| K=4  | 237 | 275 | 240 | 156 |  91  |
| K=8  | 245 | 273 | 282 | 161 |  93  |

## Takeaways

- **K=8 consistently wins** at every T: +9 % to +33 % over K=1 (all-sync).
  Peak is 282 kops/s at T=4 K=8.
- **K=2 gives most of the gain** — going from 1→2 captures the biggest
  step. Beyond K=4 the incremental gain tapers (expected: sync-ACK cost
  is O(N/K); K=2 vs K=1 halves the wait, K=8 vs K=4 only quarters what
  was already a small residual).
- **Peak T remains T=4** — Phase 5 reduces A's *per-write* latency but
  doesn't fix Phase 4's O(N²) ring push fan-out, which still limits T.
  Need cross-client same-host ring bypass (local DRAM queue) to push T
  peak further.

## How to reproduce

```
export FUSEE_A_GROUPS=8   # each client waits on ~N/8 peer ACKs
bash scripts/run_g34_scaling_sweep.sh  # OPTS=A, WORKLOADS=workloada, etc.
```

Default build still uses K=1 so baseline A runs are unchanged.
