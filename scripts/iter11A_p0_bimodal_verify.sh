#!/usr/bin/env bash
# iter-11A Phase 0 — bimodal-cell investigation.
# Run 5 known-bimodal cells × 20 reps each, capture per-rep:
#   - trans_agg_thpt
#   - first_op_ns_{max,avg}
#   - ops_to_first_ns_{max,avg}
# Output one TSV row per (cell, rep). Downstream analysis: cluster
# health vs collapse reps, see if first-op latency systematically
# differs.
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.tsv"
RAW="$OUTDIR/raw"
mkdir -p "$RAW"

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0
NB=65536
OPS=50000
TIMEOUT_S=120
COMMON_ENV="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0"

# 5 known-bimodal cells (from iter-10A Phase 5.B verification)
declare -a CELLS=(
  "workloada 4 on 512"
  "workloada 64 on 1024"
  "workloadb 64 on 512"
  "workloadd 4 off 512"
  "workloadf 8 on 512"
)
REPS=20

echo -e "cell\trep\ttrans_agg_thpt\tw_p99_ns\tr_p99_ns\tfirst_op_ns_max\tfirst_op_ns_avg\tops_to_first_ns_max\tops_to_first_ns_avg" > "$SUMMARY"

total=$(( ${#CELLS[@]} * REPS ))
done=0

for cell in "${CELLS[@]}"; do
  read -r WL_NAME T CACHE_MODE KV <<< "$cell"
  CACHE=0; [ "$CACHE_MODE" = "on" ] && CACHE=1
  CELL_LBL="${WL_NAME}_T${T}_${CACHE_MODE}_kv${KV}"

  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh -n g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    ssh -n g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    wait
    sleep 0.2
    cmd_h0="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL_NAME timeout $TIMEOUT_S $BIN $DEV $WL/${WL_NAME}.spec_load $WL/${WL_NAME}.spec_trans $NB $OPS"
    cmd_h1="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL_NAME timeout $TIMEOUT_S $BIN $DEV $WL/${WL_NAME}.spec_load $WL/${WL_NAME}.spec_trans $NB $OPS"
    h0_log="$RAW/${CELL_LBL}_rep${rep}_h0.log"
    h1_log="$RAW/${CELL_LBL}_rep${rep}_h1.log"
    ssh -n g3 "$cmd_h0" >$h0_log 2>&1 &
    ssh -n g4 "$cmd_h1" >$h1_log 2>&1 &
    wait

    # Parse fields from h0 line
    line=$(grep '^YCSB' $h0_log | head -1)
    thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
    wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+' | cut -d= -f2)
    rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+' | cut -d= -f2)
    fo_max=$(echo "$line" | grep -oE 'first_op_ns_max=[0-9]+' | cut -d= -f2)
    fo_avg=$(echo "$line" | grep -oE 'first_op_ns_avg=[0-9]+' | cut -d= -f2)
    of_max=$(echo "$line" | grep -oE 'ops_to_first_ns_max=[0-9]+' | cut -d= -f2)
    of_avg=$(echo "$line" | grep -oE 'ops_to_first_ns_avg=[0-9]+' | cut -d= -f2)
    : "${thpt:=0}"; : "${wp99:=0}"; : "${rp99:=0}"
    : "${fo_max:=0}"; : "${fo_avg:=0}"; : "${of_max:=0}"; : "${of_avg:=0}"
    echo -e "$CELL_LBL\t$rep\t$thpt\t$wp99\t$rp99\t$fo_max\t$fo_avg\t$of_max\t$of_avg" >> "$SUMMARY"
    done=$((done + 1))
    mops=$(awk "BEGIN{printf \"%.3f\", $thpt/1000000}")
    echo "[bimodal] $done/$total: $CELL_LBL rep=$rep thpt=$mops Mops/s first_op_max=${fo_max}ns ops_to_first_max=${of_max}ns"
  done
done

echo "[bimodal] DONE → $SUMMARY"
