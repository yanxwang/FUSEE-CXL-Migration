# P5 copy elimination attribution framework

**Phase**: P5
**Question**: iter-13A delivered HAZARD direct-pool read (P1) + W1 reserved
per-host segments (P2), eliminating data copies on read and write paths.
Yet trans_agg_thpt did not improve materially over the STAGING baseline
(both ~17–19 Mops/s on read-heavy YCSB-c/b). **Why?**

This phase produces a quantitative answer choosing among 4 cases.

## Builds

- **A_baseline** = `build-cxl` (FUSEE_READ_GUARD=0, FUSEE_WRITE_ALLOC=0 → STAGING)
- **B_production** = `build-cxl-w1` (FUSEE_READ_GUARD=2 HAZARD, FUSEE_WRITE_ALLOC=1 W1 RESERVED)

Both with FUSEE_LRU_SAMPLE=0 (default — F2 ROLLBACK confirmed in P4.3).

## Cells

| Workload | T | KV | Cache | Reps | Tool |
|---|---|---|---|---|---|
| workloada (R50W50 Zipf) | 64 | 1024 | on | 5 | protocol_a_ycsb + pcm-memory |

(workloada chosen because it exercises BOTH eliminated copies — read path
via cache miss → forward_read, write path via cross-host writes. workloadc
pure-read would only exercise the read elimination.)

## Measurements per rep

1. trans_agg_thpt (Mops/s) — from h0 stdout YCSB line
2. DRAM read BW (MBps) — pcm-memory avg over run window, system aggregate
3. DRAM write BW (MBps) — pcm-memory avg over run window, system aggregate

## Cases

|  | BW drop ≥ 10% | BW flat (< 10% delta) |
|---|---|---|
| **Thpt up ≥ 5%** | **case C** — copy elim worked + bottleneck moved (expected design intent) | **case D** — thpt gain unrelated to BW; suspect lock / scheduling |
| **Thpt flat** | **case A** — copy elim worked at BW level but downstream stage is new bottleneck (e.g., MESI ping-pong on cache_pool_insert W10) | **case B** — copy elim ineffective: data was cache-hot anyway, or HAZARD/W1 overhead canceled the savings |

(thpt regression ≥ 5% would be a separate "case R" — would indicate net
overhead from HAZARD/W1 instrumentation; iter-13A measurements already
ruled this out within noise.)

## Acceptance bar for P5

Per CLAUDE.md (user requirement during planning):
- Phase 5 must produce a **quantitative case A/B/C/D conclusion**.
- Quantitative = numeric BW delta + numeric thpt delta + classification per
  the table above.
- A "case B" conclusion satisfies the bar — we don't need to "find a win";
  attribution itself is the deliverable.

## Predicted case (going in)

Based on path_decomp (P4):
- W10 = cache_pool_insert MESI ping-pong on 1088B entry = 3.54µs p50 (3.5×
  spec). This is structural and applies to both A_baseline and B_production.
- R2hit = 0.23µs (7.7× spec) for cache hits — F2 attempted, no gain.
- HAZARD adds per-read hazard_protect+release (per cell store + sfence).
  W1 adds per-write reserved-segment lookup + direct pool write.
- Read fast-path under workloada cache=on hits the TLS L0 (5% miss → forward).
  So HAZARD savings only kick in on the 5% miss → tiny fraction.
- Write path: W1 saves owner→staging→pool copy (1024B) but the receiver
  still does cache_pool_insert (which is the W10 anomaly).

Prediction: **case A or case B**. We expect either (a) BW dropped (the 1024B
write copy is gone) but thpt is W10-bound, or (b) BW flat (forward_write
volume was modest relative to memory traffic from cache_pool_insert +
TLS evictions + general MESI traffic).

Either case A or B is a defensible iter-13A retrospective answer that
justifies iter-15A targeting W10 as the new structural bottleneck.

## Output artifact

`docs/iter14A_p5_attribution_*/conclusion.md` with the filled-in case
table + raw numbers + iter-15A implication.
