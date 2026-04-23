#!/usr/bin/env bash
# Promote a logs/g34_scaling_sweep_C_only_<ts>/ sweep into a spec-compliant
# docs/g34_scaling_ycsb_C_only_<ts>/ deliverable. Run this AFTER the sweep
# finishes.
#
# Usage:
#   scripts/finalize_c_only_sweep.sh <sweep_log_dir> [<iter_label>]
#
# Side effects:
#   - creates docs/g34_scaling_ycsb_C_only_<ts>/ mirror of the log dir
#   - runs docs/plot_scaling_sweep.py to emit the 30-plot C subset
#   - runs docs/plot_c_compare.py with baseline + this run for the extra/ dir
#   - writes plot_commit.txt, iteration_note.md placeholder
# The caller still has to fill in iteration_note.md with real content.
set -euo pipefail

src="${1:?usage: $0 <sweep_log_dir> [<iter_label>]}"
label="${2:-iter1_perslot_lfm}"
ts=$(basename "$src" | sed -E 's/^.*_([0-9]{8}_[0-9]{6}).*/\1/')
dst="$HOME/FUSEE/docs/g34_scaling_ycsb_C_only_${ts}"
mkdir -p "$dst/extra" "$dst/cache_off"

cp "$src/SUMMARY.log" "$dst/SUMMARY.log"

{
  echo "git_sha=$(git -C "$HOME/FUSEE" rev-parse HEAD)"
  echo "branch=$(git -C "$HOME/FUSEE" rev-parse --abbrev-ref HEAD)"
  echo "status=$(git -C "$HOME/FUSEE" status --short | head -20)"
  echo "ts=$ts"
  echo "host=$(hostname)"
  echo "opts=C"
  echo "workloads=workloada workloadb workloadc workloadd workloadf"
  echo "threads=1 2 4 8 16 32 64 86"
  echo "cache_modes=on off"
  echo "num_buckets=65536"
  echo "max_ops=200000"
} > "$dst/plot_commit.txt"

# Per-spec 30-plot set (C only; A and B grids will just be empty).
python3 "$HOME/FUSEE/docs/plot_scaling_sweep.py" \
    "$dst/SUMMARY.log" "$dst" --cache=on 2>&1 | tail -3 || true
python3 "$HOME/FUSEE/docs/plot_scaling_sweep.py" \
    "$dst/SUMMARY.log" "$dst/cache_off" --cache=off 2>&1 | tail -3 || true

# Extra overlay: this iter vs baseline (p2_v4) vs iter1 per-slot LFM if present.
baseline_log="$HOME/FUSEE/logs/g34_scaling_sweep_p2_v4_20260422_205644/SUMMARY.log"
iter1_log="$HOME/FUSEE/docs/g34_scaling_ycsb_C_only_20260423_051200/SUMMARY.log"
cmpargs=()
[ -f "$baseline_log" ] && cmpargs+=("baseline (LFM per-bucket)" "$baseline_log")
if [ -f "$iter1_log" ] && [ "$dst/SUMMARY.log" != "$iter1_log" ]; then
  cmpargs+=("iter1 per-slot LFM" "$iter1_log")
fi
cmpargs+=("$label" "$dst/SUMMARY.log")
if [ ${#cmpargs[@]} -ge 4 ]; then
  python3 "$HOME/FUSEE/docs/plot_c_compare.py" "$dst/extra" "${cmpargs[@]}" 2>&1 | tail -3 || true
fi

cat > "$dst/iteration_note.md" <<EOF
# iter1 / step 3 — C-only scaling sweep (per-slot LFM)

- Timestamp: $ts
- Decomp + optimization that produced this sweep:
  - Instrumentation in \`src/cxl_kv_ops_C.cc\` gated by \`FUSEE_LATENCY_DECOMP=1\`.
  - Per-slot LFM lock (7 × \`shm_mutex_t\` per bucket) enabled via
    \`FUSEE_PER_SLOT_LOCK=ON\`, \`FUSEE_USE_TICKET_LOCK=OFF\`.
- Baseline comparison: \`logs/g34_scaling_sweep_p2_v4_20260422_205644\`.
- Target: \`trans_agg_thpt >= 20 Mops/s\` on C at some T for each of
  workloada, workloadb, workloadf.
EOF

echo "$dst"
