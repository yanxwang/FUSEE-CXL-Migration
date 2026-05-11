#!/usr/bin/env bash
# iter-10A Phase 5.B — 5-rep verification on anomaly cells
# (per CLAUDE.md §13 gate-5 anomaly-scan-triggered review).
#
# Re-runs each anomaly cell 5 times; reports min / median / max
# throughput so we can distinguish single-rep init noise from real
# bottleneck regressions.
set -u

OUTDIR="${1:?usage: $0 <output_dir> <cells_file>}"
CELLS="${2:?usage: $0 <output_dir> <cells_file>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.log"
RAW="$OUTDIR/raw"
mkdir -p "$RAW"
: > "$SUMMARY"

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0
NB=65536
OPS=50000
TIMEOUT_S=120
COMMON_ENV="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0"

echo "wl T cache kv rep1 rep2 rep3 rep4 rep5 median_mops max_mops" >> "$SUMMARY"
total=$(wc -l < "$CELLS"); done=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  [[ "$line" =~ ^# ]] && continue
  read -r WL_NAME T CACHE_MODE KV <<< "$line"
  CACHE=0; [ "$CACHE_MODE" = "on" ] && CACHE=1
  CELL_LBL="${WL_NAME}_T${T}_${CACHE_MODE}_kv${KV}"
  reps=()
  for rep in 1 2 3 4 5; do
    cookie=$(date +%s%N)
    ssh -n g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    ssh -n g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    wait; sleep 0.2
    cmd_h0="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL_NAME timeout $TIMEOUT_S $BIN $DEV $WL/${WL_NAME}.spec_load $WL/${WL_NAME}.spec_trans $NB $OPS"
    cmd_h1="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL_NAME timeout $TIMEOUT_S $BIN $DEV $WL/${WL_NAME}.spec_load $WL/${WL_NAME}.spec_trans $NB $OPS"
    g0_log=$RAW/${CELL_LBL}_rep${rep}_h0.log
    g1_log=$RAW/${CELL_LBL}_rep${rep}_h1.log
    ssh -n g3 "$cmd_h0" >$g0_log 2>&1 &
    ssh -n g4 "$cmd_h1" >$g1_log 2>&1 &
    wait
    thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' $g0_log | head -1 | cut -d= -f2)
    [ -z "$thpt" ] && thpt=0
    reps+=("$thpt")
  done
  # median
  sorted=$(printf "%s\n" "${reps[@]}" | sort -n)
  median=$(echo "$sorted" | awk 'NR==3{print}')
  maxt=$(echo "$sorted" | tail -1)
  median_mops=$(awk "BEGIN{printf \"%.4f\", $median/1000000}")
  max_mops=$(awk "BEGIN{printf \"%.4f\", $maxt/1000000}")
  echo "$WL_NAME $T $CACHE_MODE $KV ${reps[0]} ${reps[1]} ${reps[2]} ${reps[3]} ${reps[4]} $median_mops $max_mops" >> "$SUMMARY"
  done=$((done + 1))
  echo "[verify] $done/$total: $CELL_LBL median=$median_mops max=$max_mops Mops/s"
done < "$CELLS"
echo "[verify] DONE → $SUMMARY"
