# FUSEE-CXL doc-writing conventions

Companion to [README.md](README.md). Codifies the naming + workflow
patterns we've actually used (codifying habit, not inventing new
rules). Refer here whenever creating a new doc.

---

## 1. Per-iter document cycle

Every iteration of optimization / experiment work follows this 5-step
artifact cycle:

```
  ┌─────────────────────────────────────────────────────────────┐
  │  1. PLAN    docs/iters/task_plan_<date>_<topic>.md          │
  │             (decision table + phase breakdown + scope)      │
  ├─────────────────────────────────────────────────────────────┤
  │  2. CODE    src/* changes; commits prefixed [<iter-tag>]    │
  ├─────────────────────────────────────────────────────────────┤
  │  3. SWEEP   docs/sweeps/g34_scaling_ycsb_*_<ts>/            │
  │               + iteration_note.md inside each sweep dir     │
  ├─────────────────────────────────────────────────────────────┤
  │  4. SUMMARY docs/iters/iter<N>_<topic>_summary_<date>.md    │
  │             (peaks table + analysis + follow-up)            │
  ├─────────────────────────────────────────────────────────────┤
  │  5. INDEX   append:                                         │
  │             - docs/fusee_cxl_progress.md tail (~50 lines)   │
  │             - docs/scaling_ycsb_runs_index.md (one row)     │
  │             - memory/project_<iter_topic>.md digest         │
  └─────────────────────────────────────────────────────────────┘
```

If any step is skipped, the iter is **not done**. Steps 1 and 4 are
the heaviest; 5 is the easiest to forget.

---

## 2. Naming patterns

### 2.1 Filenames (codifying current habit)

| Pattern | When | Example |
|---|---|---|
| `task_plan_<YYYYMMDD>_<topic>.md` | iter pre-plan | `task_plan_20260424_variable_kv_size.md` |
| `iter<N>_<topic>_summary_<YYYYMMDD>.md` | iter post-summary | `iter4_variable_kv_summary_20260424.md` |
| `<topic>.md` (no date) | Layer 0/1 stable refs | `design_goals.md`, `scaling_ycsb_spec.md` |
| `iteration_note.md` | inside a sweep dir (no date — dir already has ts) | `g34_scaling_ycsb_C_only_20260424_191839/iteration_note.md` |
| `g34_scaling_ycsb[_C_only]_<ts>/` | sweep deliverable dir | `g34_scaling_ycsb_C_only_20260424_191839/` |
| `<topic>_<YYYYMMDD>/` | other dated artifact dirs (hw bench, kv compare) | `hw_bench_20260424/`, `iter4_kv_compare/` |
| `<date>_<topic>_summary.md` | morning/overnight wrap-ups | `morning_summary_20260422.md` (now → `docs/archive/`) |

### 2.2 Date / timestamp formats

- Date in filename: `YYYYMMDD` (no separators) — sorts correctly with `ls`.
- Timestamp for sweeps: `YYYYMMDD_HHMMSS` from `date +%Y%m%d_%H%M%S`.
- All times in CDT (testbed local).
- **No dates inside filenames that already live in a dated parent dir**.

### 2.3 Iter numbering

Iters are numbered globally (iter 1, 2, 3, 4, 5, …) regardless of
sub-topic. The current iter number is whatever the last
`task_plan_*.md` says. New plan = next number.

### 2.4 Commit prefixes

`[<phase-or-tag>]` — matches the iter / sub-phase. Examples we've used:
- `[phase]`, `[2a]`, `[2b]`
- `[iter1]`, `[iter2]`, `[iter3]`
- `[micro-batch]`, `[kv-varlen]`, `[decomp-lfm-anatomy]`
- iter-5 style: `[iter5-mb]`, `[iter5-mf]`, `[iter5-vcache]`

Pick the most specific tag for the change. Commit prefix is your
future self's grep handle.

---

## 3. Where new files go

| Layer / kind | Location |
|---|---|
| 0 — constitution | flat `docs/` (rare changes) |
| 1 — architecture refs | `docs/refs/` |
| 2 — iter plan + summary | `docs/iters/` |
| 3 — cross-iter trackers | flat `docs/` (project-wide indexes) |
| 4 — sweep deliverables | `docs/sweeps/` (after finalize script update; pre-existing flat sweep dirs left in place) |
| 5 — ephemera | `docs/archive/` |
| Plot + pptx generator scripts | `docs/tools/` |
| **Plot style (colors, fonts, sizes)** | **`docs/tools/plot_style.py`** — single source of truth. Set 2026-04-26: **Style B = greyscale + accent red `#c44e52`**, with `axes.titlepad=10` and `bar_with_headroom()` helper for 25% top breathing room. Reference figure: `docs/iter5_kv_n_compare/iter5_bestn_thpt_bars.png`. Edit ONLY `plot_style.py` to change plot styling globally; do not put colors / rcParams inside individual `plot_*.py` scripts. **Mandatory** for every scaling_ycsb plot — every new `plot_*.py` for a sweep must `from plot_style import apply_style; apply_style()` at the top, and use `COLORS["kv256"]` etc. instead of hard-coded hex |

The 2026-04-24 cleanup migrated all flat md / log / py files to
their target subdirs (md/log → refs/iters/archive; .py → tools/).
Going forward, write directly to the right place.

**Carve-outs** (left in flat `docs/` per user 3.1/3.4 directive):
- Existing sweep / bench subdirectories: `g34_scaling_ycsb_*/`,
  `hw_bench_*/`, `g34_bench/`, `iter*_kv_compare/`, `code_references/`,
  `bell_run/`, `c1c2_run/`. **Don't enter** these to fix link rot;
  fix opportunistically when next touched for another reason.
- All `.png` / `.pptx` files stay at original locations.

**Don't fix scripts pre-emptively.** `scripts/finalize_c_only_sweep.sh`
still writes sweep dirs to flat `docs/`; many `docs/tools/*.py`
have hardcoded output paths like `docs/<name>.png` that may now
write to the wrong directory. Fix when the script is next touched
for an unrelated reason, or when it actually causes a failure.

---

## 4. Doc-writing discipline

### 4.1 One source of truth

State should live in **exactly one place**. When the same fact
appears in multiple docs, all but one is a digest pointer:

| Fact | Source of truth | Pointer locations |
|---|---|---|
| Iter peak numbers | `iter<N>_*_summary_<date>.md` | progress.md tail (digest), runs_index (one row), memory (digest) |
| Hardware baseline numbers | `design_goals.md` | memory/reference_g34_hw_baseline.md (digest) |
| Sweep procedure | `scaling_ycsb_spec.md` | every iter's task_plan references it |

When updating a number, update the source first; pointers can be
sloppy because they say "see X" but the source says the actual
number.

### 4.2 Length budgets (rough)

| Doc type | Target | Hard cap |
|---|---|---|
| `task_plan_*.md` | 200–400 lines | 600 |
| `iter*_summary_*.md` | 100–200 lines | 300 |
| `progress.md` per-iter section | 30–50 lines | 80 |
| `iteration_note.md` (inside sweep dir) | 10–20 lines | 30 |
| `runs_index.md` row | 1 row | 1 row (markdown table) |
| memory entry | 5–30 lines | 50 |

If you blow past the hard cap, it's a sign the doc is doing two
jobs — split it.

### 4.3 What to include vs link

- **Numbers, decisions, motivations**: put in the doc.
- **Code paths, function names, line numbers**: link with relative
  path (`src/cxl_kv_ops_C.cc:142`); never inline more than 5–10 lines
  of code.
- **Plots**: drop the .png alongside the .md; link with markdown
  image syntax.
- **Long log transcripts**: keep in the sweep dir, link from summary.

### 4.4 Forbidden patterns

- **Do not** auto-generate a SUMMARY-of-summaries doc that
  paraphrases the per-iter summary (they drift). Use the iter
  summary directly.
- **Do not** write the same conclusion in 3 places without a clear
  source-of-truth designation.
- **Do not** create a new top-level doc when an existing one's
  tail-append would suffice (especially `progress.md`).
- **Do not** put dates in filenames that already live in dated dirs.

---

## 5. Archive criteria

A doc moves to `docs/archive/` when **all** hold:

1. Not touched (no edit, no link from a non-archived doc) for **30
   days**.
2. Not referenced by an active task_plan or summary.
3. Not part of Layer 0–4 (constitution / refs / iter pairs / state
   trackers / sweep deliverables).

Examples that should already be archived (deferred until cleanup
sweep):
- `option_a_*.md`, `c1_c2_*.md` — RDMA-side-track artifacts
- `morning_summary_<date>.md`, `overnight_summary_<date>.md`
- `phase_*_summary_<date>.md` from early phases (≤ phase-2)
- `INDEX.md`, `SUMMARY.md`, `benchmark_index.md` — superseded by
  `README.md`

Archive ≠ delete. The file still exists, just out of the way of
the active working set.

---

## 6. Memory vs docs (recap from README §3)

| | docs/ | memory/ |
|---|---|---|
| Audience | you + future Claude | future Claude only |
| Git | tracked | not tracked |
| Source of truth? | yes | no — points to docs |

When you learn a new fact mid-conversation:
- If it changes the project: update the relevant doc in `docs/`
  AND add a digest line to memory.
- If it's a personal preference / external pointer: only memory.

Memory entries are in one of four types: `user_*.md`, `feedback_*.md`,
`project_*.md`, `reference_*.md`. Each has its own template at the
top of the harness's auto-memory instructions.

---

## 7. When in doubt

Default to **less, not more**:
- A short summary in the existing iter doc beats a new doc.
- A pointer ("see X") beats a copy.
- An archive move beats a deletion (recoverable).
- Asking the user beats inventing a new convention.

The cost of one extra doc is small; the cost of N+1 stale parallel
docs is huge. The trend line should be **flat or downward** in
top-level `docs/` count.
