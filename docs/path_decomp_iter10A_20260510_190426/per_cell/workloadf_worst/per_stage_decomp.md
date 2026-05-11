# workloadf_worst per-stage decomposition

**Cell**: workload=workloadf kv=512 T=2 cache=on
**Sweep headline**: 0.005 (bimodal recovers) Mops/s
**Build**: TLS=1024 + lock-free CAS cache_pool + B0 (worker direct multi-MPSC)

## Healthy capture: ❌ NONE