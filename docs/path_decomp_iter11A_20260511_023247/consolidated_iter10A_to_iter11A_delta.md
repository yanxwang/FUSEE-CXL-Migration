# iter-10A → iter-11A consolidated path_decomp delta

Per-Phase predicted-vs-actual gain across the 10 cells (iter-10A Phase 4's best+worst per workload).

## P1 forwarder-pool-direct (target R3)

| Cell | R3 p50 Δ% | R3 mean Δ% | R4 p50 Δ% | R4 mean Δ% |
|---|---:|---:|---:|---:|
| workloada_best | +47.4% | +47.0% | -12.2% | -15.2% |
| workloada_worst | +15.2% | +16.6% | -10.8% | -23.2% |
| workloadb_best | +33.9% | +35.6% | +80.4% | +33.8% |
| workloadb_worst | +32.9% | +33.9% | -3.4% | -9.8% |
| workloadc_best | +50.7% | +50.4% | +9.3% | +20.1% |
| workloadc_worst | — | — | — | — |
| workloadd_best | +23.8% | +24.8% | -26.8% | -65.5% |
| workloadd_worst | — | — | — | — |
| workloadf_best | +19.1% | +22.4% | -12.1% | -14.3% |
| workloadf_worst | — | — | — | — |

**Median p50 Δ across all 14 samples**: +19.1%
**Median mean Δ across all 14 samples**: +22.4%

## P2 parallel inval drain (target I6)

| Cell | I3 p50 Δ% | I3 mean Δ% | I4 p50 Δ% | I4 mean Δ% | I5 p50 Δ% | I5 mean Δ% | I6 p50 Δ% | I6 mean Δ% | I7 p50 Δ% | I7 mean Δ% | I8 p50 Δ% | I8 mean Δ% |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| workloada_best | -27.0% | +1.9% | — | — | — | — | — | — | — | — | — | — |
| workloada_worst | -26.6% | -8.0% | +7.9% | +1.7% | +25.0% | +25.8% | — | — | +40.0% | +2.9% | +200.0% | +77.8% |
| workloadb_best | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadb_worst | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadc_best | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadc_worst | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadd_best | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadd_worst | — | — | — | — | — | — | — | — | — | — | — | — |
| workloadf_best | +28.9% | -2.0% | -24.0% | -23.6% | -17.9% | -19.4% | +12.3% | +12.9% | -16.1% | +1688.2% | +6.5% | -19.6% |
| workloadf_worst | — | — | — | — | — | — | — | — | — | — | — | — |

**Median p50 Δ across all 12 samples**: +7.9%
**Median mean Δ across all 12 samples**: +1.9%

## P3 hot-bucket sharding (target R1, wl-a)

| Cell | R1 p50 Δ% | R1 mean Δ% | R2hit p50 Δ% | R2hit mean Δ% | R2miss p50 Δ% | R2miss mean Δ% |
|---|---:|---:|---:|---:|---:|---:|
| workloada_best | -2.9% | -4.8% | +0.0% | +1.4% | -2.7% | -6.3% |
| workloada_worst | +1.2% | -1.0% | +0.0% | -2.5% | +0.2% | -0.7% |
| workloadb_best | +7.8% | +4.1% | +5.8% | +13.7% | +2.0% | +1.5% |
| workloadb_worst | +2.3% | +5.0% | +0.0% | -0.7% | +2.9% | +13.7% |
| workloadc_best | +5.1% | +3.8% | +0.0% | +2.4% | -1.7% | -8.9% |
| workloadc_worst | — | — | — | — | — | — |
| workloadd_best | +7.7% | +9.8% | +0.0% | +10.2% | +92.5% | +18.9% |
| workloadd_worst | — | — | — | — | — | — |
| workloadf_best | +2.1% | +3.2% | +1.3% | +0.0% | +1.2% | +6.4% |
| workloadf_worst | — | — | — | — | — | — |

**Median p50 Δ across all 21 samples**: +1.2%
**Median mean Δ across all 21 samples**: +2.4%

## P4 RCU cache_pool (target W10)

| Cell | W10 p50 Δ% | W10 mean Δ% | W12 p50 Δ% | W12 mean Δ% |
|---|---:|---:|---:|---:|
| workloada_best | -2.0% | -2.2% | +0.6% | +22.3% |
| workloada_worst | -0.2% | -0.8% | -0.4% | -10.0% |
| workloadb_best | -0.1% | +0.8% | -0.5% | +27.5% |
| workloadb_worst | -0.1% | +3.0% | +0.0% | -24.5% |
| workloadc_best | +2.5% | -5.3% | -0.2% | +8.6% |
| workloadc_worst | — | — | — | — |
| workloadd_best | -0.9% | +1.0% | -0.3% | -21.7% |
| workloadd_worst | — | — | — | — |
| workloadf_best | -3.1% | -4.5% | -0.3% | -19.3% |
| workloadf_worst | — | — | — | — |

**Median p50 Δ across all 14 samples**: -0.2%
**Median mean Δ across all 14 samples**: -0.8%

