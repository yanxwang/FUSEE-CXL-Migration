#!/usr/bin/env bash
# iter-3A Phase 5.5 — K_channels parameter sweep.
#
# T x K grid for write-heavy workloads (A focus, optional B). Cache=on.
# Each cell runs the standard 2-host YCSB once; output is the same as
# the main scaling sweep (per-cell directory, SUMMARY.log appended).
#
# Convention: top-T per K to match the CPU pinning rule (86 - pinned):
#   K=1 -> top T 84
#   K=2 -> top T 82
#   K=4 -> top T 78
# We always include T=64 as a fixed control row.
#
# Usage:
#   bash scripts/iter3A_k_param_sweep.sh
#   OUT_ROOT=/some/path WORKLOADS="workloada" bash scripts/iter3A_k_param_sweep.sh
set -u

: "${WORKLOADS:=workloada}"
: "${TIMEOUT_S:=600}"
stamp=$(date +%Y%m%d_%H%M%S)
: "${OUT_ROOT:=$HOME/FUSEE/logs/g34_iter3A_kparam_$stamp}"
mkdir -p "$OUT_ROOT"
agg="$OUT_ROOT/SUMMARY.log"
: > "$agg"

declare -A KTOP=( [1]=84 [2]=82 [4]=78 )

echo "# iter-3A K-param sweep stamp=$stamp" | tee -a "$agg"
echo "# workloads=$WORKLOADS" | tee -a "$agg"

for K in 1 2 4; do
  topT="${KTOP[$K]}"
  for T in 64 "$topT"; do
    for wl in $WORKLOADS; do
      # Per-K CPU pinning: senders at cores [T..T+K-1]; receivers at [T+K..T+2K-1].
      env OPTS="A" \
          WORKLOADS="$wl" \
          THREADS="$T" \
          CACHE_MODES="on" \
          TIMEOUT_S=$TIMEOUT_S \
          A_SKIP_AT=999 \
          OUT_ROOT="$OUT_ROOT/K${K}_T${T}_${wl}" \
          FUSEE_PER_HOST_RING=1 \
          FUSEE_PER_SLOT_LFM_A=1 \
          FUSEE_K_CHANNELS="$K" \
          FUSEE_SENDER_BATCH_K=4 \
          FUSEE_SENDER_BATCH_T_US=20 \
          FUSEE_SENDER_CORE_BASE="$T" \
          FUSEE_RECEIVER_CORE_BASE="$((T + K))" \
          bash $(dirname "$0")/run_g34_scaling_sweep.sh 2>&1 | tee -a "$agg"
    done
  done
done

echo "# done $(date -Is)" | tee -a "$agg"
