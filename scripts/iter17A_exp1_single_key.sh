#!/usr/bin/env bash
# Exp 1: Single-key flood. ALL ops write to same key (peer-owned).
# If Plan A's bottleneck is bucket lock contention, all N values should give
# same throughput (single-receiver limit ~1 Mops).
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter17A_exp1_single_key_flood_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv
echo "T,N,routing,rep,thpt,wp50_us,wp99_us,wc_s" > $CSV

NUM_BUCKETS=8388608
TRANS_OPS=5000000
DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024
CB=131072
TRACE_DIR=/tmp/microbench_traces

run() {
  local T=$1 N=$2 routing=$3 rep=$4
  local cookie=$RANDOM$RANDOM
  local id="T${T}_N${N}_${routing}_rep${rep}"
  for h in g3 g4; do
    ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v 'pgrep' | xargs -r kill -9 2>/dev/null" >/dev/null
  done
  sleep 2
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=single_key_flood FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$routing \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE_DIR/bench_single_key_flood_h0.spec_load \
      $TRACE_DIR/bench_single_key_flood_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=single_key_flood FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$routing \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE_DIR/bench_single_key_flood_h1.spec_load \
      $TRACE_DIR/bench_single_key_flood_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  wait
  local wc=$(($(date +%s) - t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local wp50=$(grep -oP 'w_p50_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local wp99=$(grep -oP 'w_p99_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local wp50_us=$(awk -v n=${wp50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local wp99_us=$(awk -v n=${wp99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  echo "$T,$N,$routing,$rep,$thpt,$wp50_us,$wp99_us,$wc" >> $CSV
  echo "  T=$T N=$N $routing rep=$rep thpt=${thpt} wp50=${wp50_us}us wc=${wc}s"
}

# Plan A worker_id + Plan B key_hash, all N
for routing in worker_id key_hash; do
  for N in 0 4 8; do
    for T in 8 16 32 64; do
      for rep in 1 2 3; do
        run $T $N $routing $rep
      done
    done
  done
done

echo "=== Medians ==="
awk -F',' 'NR>1 {k=$1"_N"$2"_"$3; v[k]=v[k]" "$5} END{
  for(k in v){
    n=split(v[k],a," "); delete vs; for(i=1;i<=n;i++) if(a[i]!="") vs[++m]=a[i]+0;
    for(i=1;i<m;i++)for(j=i+1;j<=m;j++)if(vs[i]>vs[j]){t=vs[i];vs[i]=vs[j];vs[j]=t}
    printf "%-25s median=%.3f Mops\n", k, vs[int((m+1)/2)]/1e6; m=0
  }
}' $CSV | sort
echo OUT: $OUT
