# iter-14A Fix Register

**Started**: 2026-05-18
**Plan reference**: [task_plan_iter14A.md](task_plan_iter14A.md)
**Policy**: every clear-cause + verified-solution fix attempted;
sequential try-rollback; LOC unbounded; all attempts (PASS or FAIL)
recorded here.

---

## Entries

| Fix ID | Phase | Hypothesis | LOC | Target cells | Guardrail cells | Baseline median | Measured median | CI (95%) | Threshold | Decision | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **F1** | P2 | Cross-host write worker self-invalidates local cache before forwarding; receiver excludes src from invalidate broadcast → saves 1 InvalRing roundtrip per applicable write | ~40 | workloada T={4,32,64} c=off kv=1024 | workloadb/c T=64 c=on | (per cell) | (per cell) | (per cell) | target +5% med, CI lo > +1% | **ROLLBACK** | T=4 -13% (CI [-13, -5]) regression; T=32/64 flat; guardrails +2-3% OK. RAP overestimated invalidate-roundtrip frequency — sharer_bitmap reset to {owner} per write means most writes have no invalidates to save. Flag default OFF; code kept. Detail: [docs/iter14A_p2_xhost_write_self_inval_*/decision.md](../iter14A_p2_xhost_write_self_inval_20260519_015525/decision.md). iter-15A: revisit with sharer_bitmap retention design. |

---

## Decision legend

- **win**: target improvement above threshold, no guardrail regression → flag default ON in build
- **rollback**: target didn't improve or guardrail regressed → flag default OFF; code preserved in tree
- **research-only**: P8 entries — no code change shipped this iter; decision goes to iter-15A
