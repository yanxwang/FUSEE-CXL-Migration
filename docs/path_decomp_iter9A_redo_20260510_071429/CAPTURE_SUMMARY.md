# iter-9A redo path_decomp Phase 1+2+3 capture summary

**Cell**: workload-A KV=1024 T=64 cache=on
**Captured**: 2026-05-10T07:14
**Architecture under test**: 3-ring (Write/Read/Inval) + ForwardStaging
arena + 3 named/pinned receivers + 3 named/pinned senders + C4 startup
assert; aggregator routing OFF (default direct path)

## Healthy

- ✅ Captured at try=1: trans_agg_thpt=9801999 = **9.8020 Mops/s**
- Probes: probes_healthy/g3_try1/ + probes_healthy/g4_try1/ (134 files
  combined, 1.5M total RDTSCP frames)

## Anomaly

- ✓ **NO ANOMALY observed in 12 tries**
- All 12 retries returned 8.5–9.4 Mops/s (within 15% of healthy);
  threshold for "anomaly" was thpt < healthy/10 = 0.98 Mops/s
- The cell is reliably healthy on the iter-9A redo architecture

## Per-stage decomp

See `per_stage_decomp.md` for the full Sol-1 Healthy + Sol-4 Expected
side-by-side table, derived from the 134 probe files.

**Notable**: only W10 (directory state update + cache_pool_insert) shows
non-trivial H/E ratio (4.0× over Expected). Same finding as iter-9A
original. Carved out to iter-10A backlog #7 (lock-free cache_pool) as
a soft sub-threshold flag (H/E < 5× spec gate-5 trigger).

## Notes

- Tried with FUSEE_USE_AGGREGATOR=1 first: 0.49–0.51 Mops/s (5 tries).
  See commit a19f4cd for the analysis — single-sender-per-ring without
  batching is the new structural bottleneck. iter-10A is the right
  place to add slot batching to senders (sender reserves K consecutive
  ring slots in one fetch_add). Current default (direct path) avoids
  the bottleneck while keeping the senders alive for spec compliance.
