# G4 (4CN+4MN co-located) — Feasibility Exploration (2026-06-07)

Target: 4 machines (b1-b4) each run BOTH an MN and a CN. NUMA0(cores 0-13)=CN,
NUMA1(cores 14-27)=MN. data=idx=4, memory_num=4. Max clients = 4 CN × 7 = 28.

## Finding 1 — pure "MN memory on NUMA1" does NOT fit (blocker as specified)
Each NUMA node has only **3584 × 2MB hugepages = 7 GB**, but the MN data region is
**8 GB = 4096 hugepages**. `numactl -N1 -m1` (force MN memory to node1) → mmap can't get
4096 pages on one node → **server SIGSEGVs in ServerMM ctor** (after "kv_area_addr").
So the literal "NUMA1 for MN" with server_data_len=8GB is NOT runnable on bell.

## Finding 2 — WORKAROUND validated: CPU-split + interleaved MN memory
`numactl --cpunodebind=1 --interleave=0,1` for the MN: keeps the CPU isolation
(MN threads on node1 cores 14-17), but spreads the 8 GB across both nodes' hugepages
(3584+3584=7168 ≥ 4096). Result: **all 4 co-located MN start and stay alive.**
CN with `--cpunodebind=0 --membind=0` (node0, 7 threads).
**Probe (b1 CN 7 threads, NUMA0, vs 4 co-located MN, data=4/idx=4, workloadc):
tpt = 2.17 Mops/s, failed=0, all 4 MN survived co-residence.** → co-location works.

## Options for the user to decide (G4 config)
1. **Interleaved MN memory** (validated above): keeps CPU split, MN mem spans both nodes
   (minor cross-NUMA penalty for MN). 28 clients = 4×7. Recommended — works as-is.
2. **Reduce server_data_len to ≤7 GB** (e.g., 6 GB = 3072 pages): MN memory fits on node1
   alone → true NUMA1-local MN. But changes the 8 GB the user set, and may limit capacity
   at data=4 (4 replicas). Needs sign-off.
3. **Rebalance hugepages** (e.g., node0=2048, node1=5120 via sysfs, sudo): true node1-local
   MN (5120≥4096), CN on node0 gets 2048 (needs ~512). Asymmetric; needs sudo per host.

## Still needed for a full G4 run (deferred to discussion)
- **4-node orchestrator**: extend orch.py (currently 2-node) to 4 simultaneous CNs with
  getchar sync + sum. ylw will be useless at idx=4 (broken since idx=2). So G4 = ypaper only,
  4-node. Per-CN server_id offset (e.g., 4, 36, 68, 100) for unique client_ids.
- b4 hugepages were bumped 4096→7168 (sudo) to match b1/b3/b2 for co-location.

## Status: FEASIBLE via Option 1. Full G4 sweep awaiting user decision on config + the
4-node orchestrator build.
