#!/usr/bin/env bash
# iter-10A Phase 3 — sender batch policy comparison (B0 baseline + P0/P1/P2/P3
# scheduling policies through the aggregator). Per task_plan_iter10A.md §3.C.
#
# 5 reps per policy on workload-A T=64 cache=on KV=1024 (the iter-9A redo
# Phase 3 path_decomp cell, the historical hardest cell).
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
SUMMARY="$OUTDIR/SUMMARY.log"
RAW="$OUTDIR/raw"
mkdir -p "$OUTDIR" "$RAW"
: > "$SUMMARY"

# B0 = aggregator OFF (worker direct CXL fetch_add — current default).
# P0 = aggregator ON, sender per-slot drain (1 op per CXL fetch_add).
# P1 = aggregator ON, fixed K=16 + T_us=100 batched scheduling.
# P2 = aggregator ON, adaptive drain-all (no K cap).
# P3 = aggregator ON, per-dst round-robin (drain one dst per pass).
declare -a CONFIGS=(
  "B0:FUSEE_USE_AGGREGATOR=0"
  "P0:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P0"
  "P1:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P1"
  "P2:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P2"
  "P3:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P3"
)

REPS=5
T=64
NB=65536
OPS=200000
TIMEOUT_S=120
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb

echo "policy rep thpt_ops_per_sec mops_per_sec w_p99_us r_p99_us" >> "$SUMMARY"
total=$(( ${#CONFIGS[@]} * REPS ))
done=0
for cfg in "${CONFIGS[@]}"; do
  name="${cfg%%:*}"
  envs="${cfg#*:}"
  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.3

    g3_log=$RAW/${name}_rep${rep}_h0.log
    g4_log=$RAW/${name}_rep${rep}_h1.log

    cmd_h0="$envs FUSEE_TLS_SIZE=1024 FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloada timeout $TIMEOUT_S $BIN $DEV $WL_DIR/workloada.spec_load $WL_DIR/workloada.spec_trans $NB $OPS"
    cmd_h1="$envs FUSEE_TLS_SIZE=1024 FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloada timeout $TIMEOUT_S $BIN $DEV $WL_DIR/workloada.spec_load $WL_DIR/workloada.spec_trans $NB $OPS"

    ssh g3 "$cmd_h0" > $g3_log 2>&1 &
    ssh g4 "$cmd_h1" > $g4_log 2>&1 &
    wait

    line=$(grep "YCSB" $g3_log | head -1)
    thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
    wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+' | cut -d= -f2)
    rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+' | cut -d= -f2)
    [ -z "$thpt" ] && thpt=0
    mops=$(awk "BEGIN { printf \"%.3f\", $thpt/1000000 }")
    wp99_us=$(awk "BEGIN { printf \"%.1f\", ${wp99:-0}/1000 }")
    rp99_us=$(awk "BEGIN { printf \"%.1f\", ${rp99:-0}/1000 }")
    echo "$name $rep $thpt $mops $wp99_us $rp99_us" >> "$SUMMARY"
    done=$((done + 1))
    echo "[compare] $done/$total: policy=$name rep=$rep thpt=$mops Mops/s w_p99=${wp99_us}us r_p99=${rp99_us}us"
  done
done

echo
echo "## summary (median per policy) ##" >> "$SUMMARY"
for cfg in "${CONFIGS[@]}"; do
  name="${cfg%%:*}"
  median=$(grep "^$name " "$SUMMARY" | awk '{print $4}' | sort -n | awk 'NR==3{print}')
  printf "  policy=%-3s median_thpt=%s Mops/s\n" "$name" "$median" >> "$SUMMARY"
done
cat "$SUMMARY" | tail -10
echo "[compare] DONE; output in $OUTDIR"
