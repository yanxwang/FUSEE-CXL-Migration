# workloadd_worst per-stage decomposition

**Cell**: workload=workloadd kv=512 T=4 cache=off
**Sweep headline**: 1.78 (bimodal) Mops/s
**Build**: TLS=1024 + lock-free CAS cache_pool + B0 (worker direct multi-MPSC)

## Healthy capture: ❌ NONE