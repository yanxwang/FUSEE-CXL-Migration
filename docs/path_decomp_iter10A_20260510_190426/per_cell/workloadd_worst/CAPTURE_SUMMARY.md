# workloadd_worst capture summary

**Cell**: workload=workloadd KV=512 T=4 cache=off
**Captured**: 2026-05-10T19:34:32-05:00

## Healthy
- Captured at try=1, thpt=3072621 = 3.0726 Mops/s
- Probes: probes_healthy/g3_try1/, g4_try1/

## Anomaly
- Captured at try=2, thpt=62524 = 0.0625 Mops/s
- Threshold was thpt < 307262 (= healthy/10)
- Probes: probes_anomaly/g3_try2/, g4_try2/
