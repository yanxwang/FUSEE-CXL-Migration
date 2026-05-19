# iter-14A Progress (live-updated)

**Started**: 2026-05-18
**Branch**: `feat/cxl-migration`
**Plan**: [task_plan_iter14A.md](task_plan_iter14A.md)
**Fix register**: [iter14A_fix_register.md](iter14A_fix_register.md)

---

## Status

| Phase | State | Notes |
|---|---|---|
| **P1** Minimum preflight | ✅ done | rekey g3/g4 + baseline build + smoke 1.42 Mops/s + hash-diff 20/20 PASS + rw_race_test sig fix (commit 258f530) |
| **P2** xhost write self-inval | ✅ done (F1 ROLLBACK) | RAP + impl + hash-diff PASS + measurement: target T=4 regressed -13%, T=32/64 flat → flag default OFF. Code kept in tree. iter-15A revisit. |
| **P3** Remaining preflight | pending | |
| **P4** Production path_decomp + fix | pending | |
| **P5** Copy elimination attribution | pending | |
| **P6** Ground truth microbench | pending | |
| **P7** Full YCSB scaling sweep | pending | |
| **P8** TLS research | pending | |
| **P9** Summary + iter-15A backlog | pending | |

---

## Quick numbers

### P1 baseline (2026-05-19)
- Smoke: workload-d kv=8 T=4 cache=on → **1.42 Mops/s** trans_agg_thpt
- Hash-diff battery (5 workloads × 4 KV): **20/20 PASS** on baseline build
- Both hosts: 6 named/pinned receivers + 3 named/pinned senders verified

### P2 measurement (2026-05-19) — F1 ROLLBACK
- Hash-diff battery on build-cxl-p2: **20/20 PASS** (§I9 preserved)
- G6 rw_race_test: pre-existing-broken at baseline (build-cxl-w1 also fails); documented [docs/iter14A_p2_*/g6_pre_existing_issue.md], iter-15A backlog item
- Measurement vs build-cxl-w1:
  - workloada T=4 c=off kv=1024: **-13.18%** (CI [-13.33, -4.58]) ← target regress
  - workloada T=32: -0.40%, T=64: -1.80% — flat
  - workloadc T=64 c=on: +2.80%, workloadb T=64 c=on: +2.05% — guardrails OK
- Decision: ROLLBACK per universal fix policy. CMake `FUSEE_XHOST_WRITE_SELF_INVAL` default 0.

---

## Auto-notify events (stage spec re-stages, etc.)

(populated as events occur)

---

## Pending decisions (none waiting on user)

iter-14A runs autonomously; all decisions follow plan + fix policy.
