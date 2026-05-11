# workloadc_worst per-stage decomposition

**Cell**: workload=workloadc kv=512 T=2 cache=off
**Sweep headline**: 1.10 Mops/s
**Build**: TLS=1024 + lock-free CAS cache_pool + B0 (worker direct multi-MPSC)

## Healthy capture: ❌ NONE