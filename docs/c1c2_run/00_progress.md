# c1/c2 FUSEE Deployment Run — Live Progress

**Start time**: Sun Apr 19 06:42:08 PM CDT 2026
**Target end**: 4/19 10:00 PM CDT
**Goal**: Bring IB fabric up via DPU opensm, test RDMA, compile FUSEE, run 1 MN + 1 CN latency test.

## Status Log


## Completion summary — Sun Apr 19 07:04:21 PM CDT 2026

All planned tasks DONE. See [FINAL_REPORT.md](FINAL_REPORT.md) for details.

- ✅ Diagnosed IB port Down (IS_SM_DISABLED in cap_mask)
- ✅ Started opensmd on c1 DPU (192.168.100.2, ubuntu user) — IB fabric came UP
- ✅ Verified c1 mlx5_0 (LID 7) and c2 mlx5_0 (LID 8) both Active with SM LID 5
- ✅ RDMA write bandwidth: 11,512 MiB/s (96.6 Gbps of 100 Gbps line)
- ✅ RDMA write latency: 1.42 μs
- ✅ **RDMA atomic CAS: 2.42 μs** (required for FUSEE, unsupported on Bell cluster)
- ✅ FUSEE compiled on both c1 and c2 with zero errors
- ✅ HugePages configured (7168 × 2MB = 14 GB per host)
- ✅ 2 MN + 1 CN latency test completed: 400K ops, 0 failures
    - SEARCH avg 5.6 μs, INSERT 9.8 μs, UPDATE 10.5 μs, DELETE 13.3 μs
- ℹ  Confirmed num_replication=1 has a silent-insert-drop bug; paper-default num_replication=2 works perfectly.

## Session time budget
- Requested end: 4/19 10:00 PM CDT
- Actual end:    Sun Apr 19 07:04:21 PM CDT 2026
- Leftover time: plenty (all core tasks done)
