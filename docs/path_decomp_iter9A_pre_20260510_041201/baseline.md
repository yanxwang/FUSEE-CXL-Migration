# iter-9A Phase 0 baseline (pre-Phase 1)

**Date**: 2026-05-10T04:12:01-05:00
**Branch**: feat/cxl-migration @ baba91b
**State**: post-iter-8A, pre-iter-9A code/spec changes

## Smoke test (per task plan Phase 0 step 3)

workload-d KV=8 T=4 cache=on, MAX_OPS=50000, 2-host:
- trans_agg_thpt: **1.73 Mops/s**
- trans_wall_max: 28.98 ms
- w_avg / p50 / p99: 10.6 / 14.5 / 20.3 µs
- r_avg / p50 / p99: 5.3 / 6.5 / 16.4 µs
- result: HEALTHY (no FAIL, no anomaly)

## Reference (from iter-8A summary)

iter-8A peak workload-d KV=512 T=64 cache=on = 18.37 Mops/s.

## iter-9A Phase 0 exit

- ✅ ssh g3+g4 reachable (rekey+bootstrap done)
- ✅ workloads rsynced
- ✅ daxctl devdax + chmod 666 OK
- ✅ smoke test PASS (1.73 Mops/s)
- ✅ binaries from iter-8A still functional
