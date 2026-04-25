# RDMA NIC comparison — measured on our clusters

Consolidated perftest numbers for every RDMA NIC we've tried while bringing up FUSEE, plus the paper's reference platform.

All measurements use Mellanox's `perftest` suite (`ib_write_bw`, `ib_write_lat`, `ib_atomic_bw`, `ib_atomic_lat`) over a single RC QP, two hosts back-to-back through the local switch.

## Summary table

| Property | **FUSEE paper** (reference) | Connect-IB (bell, pre-swap) | ConnectX-4 (bell, current) | BlueField-3 (c1/c2) |
|---|---|---|---|---|
| Model number | ConnectX-3 (MT27500) | MT4113 / MT27600 | MT4115 / MT27700 | MT43244 (integrated CX-7) |
| Generation / year | 2012, FDR-era | 2012–2013, FDR HPC-only | 2014–2015 | 2022–2023 |
| Link rate | 56 Gbps FDR | 56 Gbps FDR | 100 Gbps EDR (4×25 capable) | 200 Gbps NDR200 capable |
| Link rate we saw | 56 Gbps (paper setup) | 56 Gbps FDR | **56 Gbps FDR** (capped by SX6036 switch) | **100 Gbps EDR** (c1↔switch↔c2) |
| Firmware | — | 10.16.1066 | 12.16.1020 / 12.21.2010 | 32.47.1088 |
| `atomic_cap` | `ATOMIC_HCA` ✅ | **`ATOMIC_NONE`** ❌ | `ATOMIC_HCA` ✅ | `ATOMIC_HCA` ✅ |
| FUSEE runs? | yes (paper) | **no** (hangs in `ibv_poll_cq` on first CAS) | yes | yes |

## Microbench numbers

| Metric | FUSEE paper | Connect-IB | ConnectX-4 @ FDR | BlueField-3 @ EDR |
|---|---:|---:|---:|---:|
| `ib_write_bw` (avg, MiB/s) | not reported | 5,565¹ | 5,935¹ | **11,513** |
| `ib_write_bw` effective Gbps | ~50² | 46.7 | 49.8 | **96.6** |
| `ib_write_bw` efficiency vs line rate | ~89%² | 83% | 89% | 97% |
| `ib_write_lat` (μs, 2 B payload) | not reported | **1.08** | **0.91** | 1.42 |
| `ib_atomic_bw` (Mops/s) | not reported | **N/A (fails)** | 2.37 | 1.71 |
| `ib_atomic_lat` (μs, 8 B CAS) | not reported | **N/A (fails)** | 1.74 | 2.42 |

¹ Our `ib_write_bw` outputs are in MB/s (decimal, 10⁶) for Connect-IB perftest build, MiB/s for newer builds — normalized to MiB/s here (×0.954).
² Paper doesn't publish raw ib_write_bw, but their Figure 11 throughput implies they're getting ~85–90% of 56 Gbps line rate.

## Key observations

### 1. ATOMIC_HCA is non-negotiable for FUSEE
Without it, the `kv_insert_cas_primary_sync` → `rdma_post_sr_list_batch_sync` → `ibv_poll_cq` loop never returns. That's exactly what we saw on the pre-swap Connect-IB bell cluster: INSERT hung, zero RDMA traffic on the wire, 100 % CPU spin. Swapping in ConnectX-4 (same box, same switch) fixed it immediately.

### 2. BlueField-3's latency is *slightly higher* than ConnectX-4
Counter-intuitive but explainable:
- CX-4 latency is dominated by PCIe + wire RTT at FDR
- BF-3 has an internal ASIC path through the SoC's PCIe complex that adds ~400 ns for ops to traverse the host↔DPU boundary, especially for atomic ops (which serialize on the 8-byte address).

Bandwidth-wise BF-3 still wins by 2× because the link is 100 Gbps vs 56 Gbps.

### 3. Our ConnectX-4 + SX6036 = effectively the paper's platform
Both ConnectX-3 and ConnectX-4 support `ATOMIC_HCA`; both run at 56 Gbps FDR against the SX6036 switch; both expose the same libibverbs API surface. We're running on the same *functional* hardware as the paper. Our FUSEE latency numbers on c1/c2 (BF-3) and on b1-b4 (CX-4) bracket the paper's ConnectX-3 datapoints from above and slightly below respectively.

### 4. The paper doesn't report these microbenchmarks
FUSEE FAST'23 reports application-level (YCSB) throughput and per-op latency but not raw `ib_write_bw`/`ib_atomic_lat` for their ConnectX-3 setup. Our numbers are consistent with community measurements for that generation (e.g. ConnectX-3 @ FDR usually clocks ~6,100-6,500 MB/s in `ib_write_bw`, ~1.0-1.2 μs `ib_write_lat`, ~2-3 μs `ib_atomic_lat`).

## Where these came from

- BlueField-3: [`docs/c1c2_run/03_rdma_perf_tests.log`](c1c2_run/03_rdma_perf_tests.log)
- Connect-IB: earlier bell session (see conversation history). NIC port is now `Disabled` after the CX-4 swap, so we can't re-measure.
- ConnectX-4: freshly measured on b1↔b2 (rack3-2526-1 ↔ rack3-2526-2) during current session.
- FUSEE paper: Section 6.1 Testbed, Figure 10/11/13 per-op latency and throughput.

## Command reference (if you want to re-measure)

```bash
# Bandwidth
ssh b2 "ib_write_bw -d ibp2s0f0 -D 3 &"
ssh b1 "ib_write_bw -d ibp2s0f0 -D 3 192.168.128.152"

# Write latency
ssh b2 "ib_write_lat -d ibp2s0f0 -D 3 &"
ssh b1 "ib_write_lat -d ibp2s0f0 -D 3 192.168.128.152"

# Atomic bandwidth (CAS throughput)
ssh b2 "ib_atomic_bw -d ibp2s0f0 -D 3 &"
ssh b1 "ib_atomic_bw -d ibp2s0f0 -D 3 192.168.128.152"

# Atomic latency (CAS round-trip) — this is the one FUSEE depends on
ssh b2 "ib_atomic_lat -d ibp2s0f0 -D 3 &"
ssh b1 "ib_atomic_lat -d ibp2s0f0 -D 3 192.168.128.152"
```

For BlueField-3, replace `ibp2s0f0` with `mlx5_0` and the IP with `192.168.128.32`.
