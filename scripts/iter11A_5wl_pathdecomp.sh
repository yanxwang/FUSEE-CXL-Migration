#!/usr/bin/env bash
# iter-10A Phase 4 — 5-workload × 2-cell path_decomp capture.
# Per task_plan_iter10A.md §4.
#
# Usage:
#   iter10A_5wl_pathdecomp.sh <output_dir> <cells.txt>
#
# cells.txt format (one cell per line, fields whitespace-separated):
#   <label> <workload> <kv> <T> <cache>
# example:
#   workloada_best workloada 256 64 off
#   workloada_worst workloada 1024 64 on
#   workloadb_best workloadb 256 32 on
#   ...
#
# For each cell:
#   - Healthy capture: try up to 5 times until thpt >= 0.5 * expected
#   - Anomaly capture: try up to 12 times until thpt < expected/10
set -u

OUTDIR="${1:?usage: $0 <output_dir> <cells.txt>}"
CELLS_FILE="${2:?usage: $0 <output_dir> <cells.txt>}"
mkdir -p "$OUTDIR"

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0
NB=65536
OPS=200000
TIMEOUT_S=120
COMMON_ENV="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0"

run_one() {
  # Args: label workload kv T cache_int
  local label=$1 wl=$2 kv=$3 T=$4 cache=$5
  local cookie=$(date +%s%N)
  local remote_dir=/tmp/iter11A_pd_${label//./_}
  ssh -n g3 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV" >/dev/null 2>&1 &
  ssh -n g4 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV" >/dev/null 2>&1 &
  wait
  ssh -n g3 "cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL/${wl}.spec_load $WL/${wl}.spec_trans $NB $OPS 2>&1 | grep YCSB" > /tmp/h0_pd.log &
  ssh -n g4 "cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL/${wl}.spec_load $WL/${wl}.spec_trans $NB $OPS 2>&1 > /dev/null" &
  wait
  echo "$remote_dir" > /tmp/last_remote_pd_dir
  cat /tmp/h0_pd.log
}

# Read each cell line
while IFS= read -r line; do
  [ -z "$line" ] && continue
  [[ "$line" =~ ^# ]] && continue
  read -r LABEL WL_NAME KV T CACHE_MODE EXPECTED <<< "$line"
  # cache_mode: on=1 off=0
  CACHE_INT=0; [ "$CACHE_MODE" = "on" ] && CACHE_INT=1

  CELL_DIR="$OUTDIR/per_cell/$LABEL"
  mkdir -p "$CELL_DIR/probes_healthy" "$CELL_DIR/probes_anomaly"

  echo "===== CELL $LABEL : wl=$WL_NAME kv=$KV T=$T cache=$CACHE_MODE expected~${EXPECTED} Mops/s ====="

  # Healthy threshold = 0.5 * expected (if expected given), else 0.5 Mops/s
  HEALTHY_MIN=500000
  if [ -n "${EXPECTED:-}" ]; then
    HEALTHY_MIN=$(awk "BEGIN { printf \"%d\", $EXPECTED * 1000000 * 0.5 }")
  fi

  HEALTHY_OK=0
  HEALTHY_THPT=0
  for try in 1 2 3 4 5; do
    echo "--- $LABEL healthy try $try ---"
    run_one "${LABEL}_h.$try" "$WL_NAME" "$KV" "$T" "$CACHE_INT"
    thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0_pd.log | head -1 | cut -d= -f2)
    [ -z "$thpt" ] && thpt=0
    if [ "$thpt" -ge "$HEALTHY_MIN" ]; then
      mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
      echo ">>> $LABEL HEALTHY: try $try thpt=$thpt = $mops Mops/s <<<"
      REMOTE=$(cat /tmp/last_remote_pd_dir)
      mkdir -p "$CELL_DIR/probes_healthy/g3_try${try}" "$CELL_DIR/probes_healthy/g4_try${try}"
      rsync -aq g3:${REMOTE}/ "$CELL_DIR/probes_healthy/g3_try${try}/"
      rsync -aq g4:${REMOTE}/ "$CELL_DIR/probes_healthy/g4_try${try}/"
      HEALTHY_OK=1
      HEALTHY_THPT=$thpt
      HEALTHY_MOPS=$mops
      HEALTHY_TRY=$try
      break
    fi
  done

  ANOMALY_OK=0
  ANOM_THRESHOLD=$((HEALTHY_THPT / 10))
  [ "$ANOM_THRESHOLD" -lt 100000 ] && ANOM_THRESHOLD=100000
  for try in 1 2 3 4 5 6 7 8 9 10 11 12; do
    echo "--- $LABEL anomaly try $try ---"
    run_one "${LABEL}_a.$try" "$WL_NAME" "$KV" "$T" "$CACHE_INT"
    thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0_pd.log | head -1 | cut -d= -f2)
    [ -z "$thpt" ] && thpt=0
    if [ "$thpt" -lt "$ANOM_THRESHOLD" ]; then
      mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
      echo ">>> $LABEL ANOMALY: try $try thpt=$thpt = $mops Mops/s (<$ANOM_THRESHOLD)"
      REMOTE=$(cat /tmp/last_remote_pd_dir)
      mkdir -p "$CELL_DIR/probes_anomaly/g3_try${try}" "$CELL_DIR/probes_anomaly/g4_try${try}"
      rsync -aq g3:${REMOTE}/ "$CELL_DIR/probes_anomaly/g3_try${try}/"
      rsync -aq g4:${REMOTE}/ "$CELL_DIR/probes_anomaly/g4_try${try}/"
      ANOMALY_OK=1
      ANOM_THPT=$thpt
      ANOM_MOPS=$mops
      ANOM_TRY=$try
      break
    fi
  done

  {
    echo "# $LABEL capture summary"
    echo
    echo "**Cell**: workload=$WL_NAME KV=$KV T=$T cache=$CACHE_MODE"
    echo "**Captured**: $(date -Is)"
    echo
    echo "## Healthy"
    if [ "$HEALTHY_OK" = "1" ]; then
      echo "- Captured at try=$HEALTHY_TRY, thpt=$HEALTHY_THPT = $HEALTHY_MOPS Mops/s"
      echo "- Probes: probes_healthy/g3_try${HEALTHY_TRY}/, g4_try${HEALTHY_TRY}/"
    else
      echo "- ❌ FAILED healthy after 5 tries (threshold $HEALTHY_MIN)"
    fi
    echo
    echo "## Anomaly"
    if [ "$ANOMALY_OK" = "1" ]; then
      echo "- Captured at try=$ANOM_TRY, thpt=$ANOM_THPT = $ANOM_MOPS Mops/s"
      echo "- Threshold was thpt < $ANOM_THRESHOLD (= healthy/10)"
      echo "- Probes: probes_anomaly/g3_try${ANOM_TRY}/, g4_try${ANOM_TRY}/"
    else
      echo "- ✓ NO ANOMALY observed in 12 tries (cell reliably healthy)"
    fi
  } > "$CELL_DIR/CAPTURE_SUMMARY.md"
  cat "$CELL_DIR/CAPTURE_SUMMARY.md"
done < "$CELLS_FILE"

echo "[pd] DONE; per-cell dirs in $OUTDIR/per_cell/"
