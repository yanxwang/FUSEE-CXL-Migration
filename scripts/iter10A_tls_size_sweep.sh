#!/usr/bin/env bash
# iter-10A Phase 1.E — TLS cache size sweep on workload-A KV=1024 T=64
# cache=on (the historically hardest cell + 50/50 R/U so TLS is
# stress-tested by epoch invalidation churn).
#
# Per task_plan_iter10A.md C11: ≥6 TLS sizes × 5 reps. We do
# 256/512/1024/2048/4096/8192 + an "off" baseline (size=0, falls back
# to shared cache_pool only — equivalent to iter-9A redo behavior).
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
SUMMARY="$OUTDIR/SUMMARY.log"
RAW="$OUTDIR/raw"
mkdir -p "$OUTDIR" "$RAW"
: > "$SUMMARY"

SIZES="0 256 512 1024 2048 4096 8192"
REPS=5
T=64
NB=65536
OPS=200000
TIMEOUT_S=120
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WORKLOAD=workloada
KV=1024

echo "TLS_size rep thpt_ops_per_sec mops_per_sec hit_rate_pct" >> "$SUMMARY"
total=$(( $(echo $SIZES | wc -w) * REPS ))
done=0
for size in $SIZES; do
  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.2

    g3_log=$RAW/size${size}_rep${rep}_h0.log
    g4_log=$RAW/size${size}_rep${rep}_h1.log

    cmd_h0="FUSEE_TLS_SIZE=$size FUSEE_TLS_DIAG=1 FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WORKLOAD timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${WORKLOAD}.spec_load $WL_DIR/${WORKLOAD}.spec_trans $NB $OPS"
    cmd_h1="FUSEE_TLS_SIZE=$size FUSEE_TLS_DIAG=1 FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WORKLOAD timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${WORKLOAD}.spec_load $WL_DIR/${WORKLOAD}.spec_trans $NB $OPS"

    ssh g3 "$cmd_h0" > $g3_log 2>&1 &
    ssh g4 "$cmd_h1" > $g4_log 2>&1 &
    wait

    thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' $g3_log | head -1 | cut -d= -f2)
    if [ -z "$thpt" ]; then thpt=0; fi
    mops=$(awk "BEGIN { printf \"%.3f\", $thpt/1000000 }")

    # aggregate TLS hit rate across all workers from g3+g4
    if [ "$size" = "0" ]; then
      hit_rate="N/A"
    else
      hit_rate=$(awk -F'hits=| misses=| hit_rate=|% ' '/tls w/{hits+=$2; misses+=$3} END{tot=hits+misses; if(tot) printf "%.1f", 100.0*hits/tot; else printf "N/A"}' $g3_log $g4_log)
    fi
    echo "$size $rep $thpt $mops $hit_rate" >> "$SUMMARY"
    done=$((done + 1))
    echo "[sweep] $done/$total: size=$size rep=$rep thpt=$mops Mops/s hit=$hit_rate%"
  done
done

echo "## summary (median per size) ##" >> "$SUMMARY"
for size in $SIZES; do
  median=$(grep "^$size " "$SUMMARY" | awk '{print $4}' | sort -n | awk 'NR==3{print}')
  hr_med=$(grep "^$size " "$SUMMARY" | awk '{print $5}' | sort -n | awk 'NR==3{print}')
  echo "size=$size  median_thpt=$median Mops/s  median_hit=$hr_med%" >> "$SUMMARY"
done
echo
echo "[sweep] DONE; output in $OUTDIR"
