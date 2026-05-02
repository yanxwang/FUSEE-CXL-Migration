# FUSEE-CXL `docs/` — Navigation

This is the **current** navigation map for FUSEE-CXL project docs.
Older index files (`INDEX.md`, `SUMMARY.md`, `benchmark_index.md`)
predate the per-iter workflow and are kept for archival reference
only — start here, not there.

> 2026-04-24 已完成一次大整理：所有 md / log / py 文件按下面 5 层
> 重新归位到 `refs/` `iters/` `sweeps/` `archive/` `tools/` 子目录。
> **已存在的子目录**（`g34_scaling_ycsb_*/`、`hw_bench_*/`、`g34_bench/`
> 等 sweep 与 bench 交付物）保持原位不动；其内部的 `.md` 链接可能
> 仍指向 `docs/<old>.md`，会按需在下次触碰它们时修复。`.png` 文件
> 保持原位，未来若运行路径出问题再修。详见
> [CONTRIBUTING.md](CONTRIBUTING.md) §3 的归位规则。

---

## 0. First-stop reading order (for a new Claude session)

Already loaded automatically by tooling:
1. **`/home/yanwang/FUSEE/CLAUDE.md`** — project rules
2. **`docs/design_goals.md`** — 20 Mops/s target, hardware baseline,
   analysis discipline
3. **`docs/scaling_ycsb_spec.md`** — canonical sweep procedure
4. `~/.claude/projects/-home-yanwang-FUSEE/memory/MEMORY.md` —
   private cross-conversation memory index

Read on demand:

5. **`docs/refs/optimization_methodology.md`** — iterative-systems-
   optimization workflow; mandatory for anyone planning a new iter
6. `docs/iters/protocol_c_retrospective.md` — consolidated 5-iter
   review of the closed C focus (also says what's left for C)
7. `docs/fusee_cxl_progress.md` — tail section is the latest iter
8. The active iter's `task_plan_<date>_<topic>.md` (see §2 below)
9. `docs/iters/iter<N>_*_summary_<date>.md` — most recent iter conclusion
10. `docs/scaling_ycsb_runs_index.md` — sweep timestamp index

---

## 1. Document layers (5 layers, by purpose)

### Layer 0 — Project constitution (override-everything)

| Path | Purpose | Update cadence |
|---|---|---|
| `docs/design_goals.md` | North-star (20 Mops/s), 3-layer hw ceiling stack, analysis discipline | rare; only after major calibration / target change |
| `docs/scaling_ycsb_spec.md` | Canonical sweep procedure (240 runs, 30 plots, dir layout) | rare; after spec evolves |
| `docs/refs/optimization_methodology.md` | **Iterative systems-optimization methodology** (per-iter loop, diagnostic toolbox, hypothesis discipline, anti-patterns) — distilled from 5 iters of Protocol C work, reusable for A/B and beyond | rare; after a focus topic closes with new lessons |
| (root) `CLAUDE.md` | Project rules for Claude harness | rare |

### Layer 1 — Architecture / protocol references

| Path | Purpose |
|---|---|
| `docs/refs/cxl_implementation_guide.md` | RDMA → CXL migration guide (early phases) |
| `docs/refs/ABC_throughput_improvement_plan.md` | A / B / C protocol definitions + optimization roadmap |
| `docs/refs/consensus_transformation_explained.md` | LFM consensus design |
| `docs/refs/cxl_architecture_plan.md` | Architecture plan (LFM revision 2) |
| `docs/refs/sync_workflow.md` | 4-way sync workflow (local / emr / g3 / g4) |

These are stable references; touched only when architecture changes.
**New refs going forward → `docs/refs/`**.

### Layer 2 — Per-iter planning + analysis pair

Each iter has **two** docs (and one progress.md tail entry):

| Pattern | Role | Example |
|---|---|---|
| `task_plan_<date>_<topic>.md` | Pre-iter design (decision table + phase breakdown) | `task_plan_20260424_iter5_multiflusher_valuecache.md` |
| `iter<N>_*_summary_<date>.md` | Post-iter results + analysis | `iter4_variable_kv_summary_20260424.md` |
| `<sweep_dir>/iteration_note.md` | Per-sweep terse note (inside sweep deliverable) | `g34_scaling_ycsb_C_only_20260424_191839/iteration_note.md` |

Active iter task_plans (chronological), all under `docs/iters/`:

**Protocol C focus (closed 2026-04-25)**:
- `task_plan_20260423_c_writepath.md` — iter 1 / 2
- `task_plan_20260424_lock_decomp_microbatching.md` — iter 3
- `task_plan_20260424_variable_kv_size.md` — iter 4
- `task_plan_20260424_iter5_multiflusher_valuecache.md` — iter 5

**Protocol A focus (open 2026-04-25)**:
- `task_plan_20260425_iter1A_baseline_decomp.md` — iter 1A (Phases 1–5a landed; 5b/6/7 deferred)
- `task_plan_20260426_iter2A_perhost_wire_compress.md` — iter 2A (Phase 1 wire **REVERTED 2026-04-27**: violated strict A semantics + mis-implemented architecture; superseded by iter-2A-revised below)
- ~~`task_plan_20260427_per_host_ring_decomp.md`~~ — **OBSOLETE** (was diagnostic on iter-2A wire; iter-2A wire reverted, no longer applicable)
- `task_plan_20260427_iter2A_revised_n11n_atomic.md` — iter 2A-revised (8 phases COMPLETE 2026-04-27; A peak 1.27 Mops/s @ kv8/T=4 cache=on = 2.4× iter-1A; 5 Mops/s target falsified)
- `task_plan_20260428_iter3A_per_slot_lfm_multi_thread.md` — **iter 3A (active)** — per-slot LFM port from C + K-channel multi-thread sender/receiver + 5 sender/receiver probes + cross-host hash-diff correctness battery

iter summary docs (also under `docs/iters/`):
- `iter3_extended_summary_20260424.md` — iter 3
- `iter4_variable_kv_summary_20260424.md` — iter 4
- `latency_decomp_C_iter{1,2,3}_*.md` — per-iter stage decomposition data
- `protocol_c_retrospective.md` — **consolidated review of all 5 C iters + outstanding C work**

### Layer 3 — Cross-iter state tracking (long-lived, append-only)

| Path | Role |
|---|---|
| `docs/fusee_cxl_progress.md` | High-level progress; tail gets one section per iter |
| `docs/scaling_ycsb_runs_index.md` | One row per sweep (ts + sha + notes) |
| `docs/fusee_cxl_session_log.md` | Session diary — **dormant since 2026-04-21**, kept for history |

These stay at flat `docs/` permanently (they're indexes, not iter
artifacts).

### Layer 4 — Sweep deliverables (one dir per sweep)

| Pattern | Role |
|---|---|
| `g34_scaling_ycsb_<ts>/` | Full A+B+C sweep |
| `g34_scaling_ycsb_C_only_<ts>/` | C-only sweep (current iter focus) |

Each contains: `SUMMARY.log`, `C_thpt_<wl>.png × 5`, `C_lat_<wl>_<r/w>.png × 9`, `cache_off/...`, `extra/...`, `plot_commit.txt`, `iteration_note.md`.

**Existing sweep dirs stay in place** at flat `docs/`. **New sweep
dirs going forward → `docs/sweeps/`** (after finalize script update;
current `scripts/finalize_c_only_sweep.sh` still writes to flat
`docs/` — fix when next touched).

### Layer "tools" — Plot + presentation generators

`docs/tools/` holds all 31 `.py` scripts (plot generators,
`gen_*_pptx.py` slide builders). Callers reference them as
`docs/tools/<name>.py`:
- `scripts/finalize_c_only_sweep.sh` → `plot_scaling_sweep.py`,
  `plot_c_compare.py`
- iter summary docs → `plot_iter3_lfm_anatomy.py`,
  `plot_iter4_peak_bars.py`, `plot_kv_compare.py`, etc.

New plot / generator scripts → `docs/tools/`. Internal hardcoded
output paths (e.g. `out = "docs/some_plot.png"`) inside scripts
**have not been pre-emptively migrated**; fix when the script is
next run.

### Layer 5 — Ephemera (one-off artifacts; archive when stale)

Already moved to `docs/archive/` on 2026-04-24:
- `morning_summary_<date>.md`, `overnight_summary_<date>.md`
- `phase_*_summary_<date>.md` (early phases)
- `cxl_abc_results.md`, `option_a_*.md`, `c1_c2_*.md`,
  `rdma_nic_comparison.md` (from RDMA side-tracks)
- `INDEX.md`, `SUMMARY.md`, `benchmark_index.md` (superseded by
  this README)
- 13 flat `.log` files (early bench logs)

Still in flat `docs/` (NOT archived; user requested existing
subdirs and `.png` / `.pptx` files stay):
- `iter*_kv_compare/` (cross-size plot dirs)
- `hw_bench_<date>/`
- All `.png` / `.pptx` files

(`.py` files **were** moved on 2026-04-24 → `docs/tools/`.)

**Archive criterion for future writes**: not touched in 30 days
AND no active iter references it → move to `docs/archive/`. See
[CONTRIBUTING.md](CONTRIBUTING.md) §5.

---

## 2. Directory structure (after 2026-04-24 cleanup)

```
docs/
├── README.md                  ← this file (top-level navigation)
├── CONTRIBUTING.md            ← doc-writing conventions
├── design_goals.md            ← Layer 0 constitution
├── scaling_ycsb_spec.md       ← Layer 0 constitution
├── fusee_cxl_progress.md      ← Layer 3 cross-iter state
├── scaling_ycsb_runs_index.md ← Layer 3 sweep index
├── fusee_cxl_session_log.md   ← Layer 3 (dormant since 2026-04-21)
│
├── refs/        Layer 1 — architecture/protocol refs (5 files)
├── iters/       Layer 2 — per-iter task_plan + summary + decomp (9 files)
├── sweeps/      Layer 4 — new sweep deliverable dirs (empty; legacy
│                              g34_scaling_ycsb_*_<ts>/ stay at flat docs/
│                              until finalize script is updated)
├── archive/     Layer 5 — superseded indexes, RDMA side-tracks,
│                              ephemeral summaries, early bench logs (27 files)
├── tools/       — plot + pptx generator scripts (31 .py files)
│
└── (untouched per user request):
    g34_scaling_ycsb_*_<ts>/, hw_bench_<date>/, g34_bench/,
    iter*_kv_compare/, code_references/, bell_run/, c1c2_run/
    + all *.png / *.pptx
```

Only the 7 flat-level files above stay at `docs/` root. Everything
else either moved to a subdir or was deliberately left in an
existing dir per user instruction.

---

## 3. Memory system (separate from docs/)

Memory lives at `~/.claude/projects/-home-yanwang-FUSEE/memory/` —
**not** under `docs/`, **not** in git. Purpose: cross-conversation
private cache for Claude (project state digests, user preferences,
external resource pointers).

Boundary:
- `docs/` is project deliverable, git-tracked, written for **you +
  future Claude**.
- `memory/` is private cache, not git-tracked, written for **future
  Claude only**.

If a fact belongs in both (e.g. iter-4 outcome), it goes in `docs/`
as the source of truth, then a **digest pointer** in memory:
"see docs/iters/iter4_variable_kv_summary_20260424.md".

---

## 4. Quick "where do I write X?" lookup

| Writing... | Where |
|---|---|
| New iter task plan | `docs/iters/task_plan_<date>_<topic>.md` |
| New iter summary | `docs/iters/iter<N>_<topic>_summary_<date>.md` |
| Per-sweep note (inside sweep dir) | `<sweep_dir>/iteration_note.md` (no date in filename — dir already has ts) |
| Update of overall progress | append a `## <date> — iter N — <topic>` section to `docs/fusee_cxl_progress.md` |
| Sweep result row | append to `docs/scaling_ycsb_runs_index.md` |
| New architecture / protocol ref | `docs/refs/<topic>.md` |
| New plot / generator script | `docs/tools/<name>.py` |
| New sweep deliverable dir | `docs/sweeps/g34_scaling_ycsb_*_<ts>/` (when finalize script is updated) |
| Cross-conversation fact for Claude | `~/.claude/projects/.../memory/<type>_<topic>.md` + index entry |
| One-off morning/overnight summary | `docs/archive/<date>_<topic>.md` (skip flat `docs/` from now on) |

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full conventions
(naming, append rules, doc discipline).
