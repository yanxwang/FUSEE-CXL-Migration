#!/usr/bin/env bash
# iter-13A Phase 2.3 — dual-track write-path comparison.
# 5 representative cells × 5 reps × 3 builds (HAZARD-baseline + W1 + W3 K=256)
# = 75 runs.
set -u

OUTDIR="${1:?usage: $0 <out_dir>}"
REPS=5
W3_K="${W3_K:-256}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.tsv"
echo -e "build\twl\tT\tcache\tkv\trep\twall_s\tthpt_mops\tw_avg_us\tr_avg_us\tw_p99_us\tr_p99_us" > "$SUMMARY"

CELLS=(
  "workloada 4 off 512"
  "workloada 32 off 1024"
  "workloadb 32 off 256"
  "workloadc 64 off 1024"
  "workloadf 32 off 512"
)
BUILDS="hazard w1 w3"
NUM_BUCKETS=65536
MAX_OPS=200000
TIMEOUT_S=120
WL_DIR=/root/FUSEE_CXL/setup/workloads
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2"

for cell in "${CELLS[@]}"; do
  set -- $cell
  WL=$1; T=$2; CACHE_STR=$3; KV=$4
  if [ "$CACHE_STR" = "on" ]; then CACHE=1; else CACHE=0; fi
  for build in $BUILDS; do
    bdir=build-cxl-$build
    BIN=/root/FUSEE_CXL/$bdir/tests/protocol_a_ycsb
    K_ENV=""
    [ "$build" = "w3" ] && K_ENV="FUSEE_BATCH_K=$W3_K"
    for rep in $(seq 1 $REPS); do
      ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0' &
      ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0' &
      wait
      sleep 0.2
      cookie=$(date +%s%N)
      ARGS="FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL $K_ENV"
      EXTRA="FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep"
      cmd0="cd /root/FUSEE_CXL/$bdir && $COMMON $ARGS $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
      cmd1="cd /root/FUSEE_CXL/$bdir && $COMMON $ARGS $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
      ssh g3 "$cmd0" > /tmp/p2_g3.$$ 2>&1 &
      p0=$!
      ssh g4 "$cmd1" > /tmp/p2_g4.$$ 2>&1 &
      p1=$!
      wait $p0; wait $p1
      line=$(grep -E "^YCSB opt=A" /tmp/p2_g3.$$ | tail -1)
      if [ -z "$line" ]; then
        echo -e "$build\t$WL\t$T\t$CACHE_STR\t$KV\t$rep\tFAIL\t0\t0\t0\t0\t0" >> "$SUMMARY"
      else
        wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
        thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
        w_avg=$(echo "$line" | grep -oE "w_avg_ns=[0-9]+" | cut -d= -f2)
        r_avg=$(echo "$line" | grep -oE "r_avg_ns=[0-9]+" | cut -d= -f2)
        w_p99=$(echo "$line" | grep -oE "w_p99_ns=[0-9]+" | cut -d= -f2)
        r_p99=$(echo "$line" | grep -oE "r_p99_ns=[0-9]+" | cut -d= -f2)
        mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
        echo -e "$build\t$WL\t$T\t$CACHE_STR\t$KV\t$rep\t$wall\t$mops\t$(awk -v t=$w_avg 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$r_avg 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$w_p99 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$r_p99 'BEGIN{printf "%.3f",t/1000}')" >> "$SUMMARY"
      fi
      rm -f /tmp/p2_g3.$$ /tmp/p2_g4.$$
      echo "[p2cmp] $build $WL T=$T $CACHE_STR kv=$KV rep=$rep done" >&2
    done
  done
done
echo "[p2cmp] all done. SUMMARY: $SUMMARY" >&2
