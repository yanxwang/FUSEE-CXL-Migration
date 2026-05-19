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

(populated as fixes are attempted)

---

## Decision legend

- **win**: target improvement above threshold, no guardrail regression → flag default ON in build
- **rollback**: target didn't improve or guardrail regressed → flag default OFF; code preserved in tree
- **research-only**: P8 entries — no code change shipped this iter; decision goes to iter-15A
