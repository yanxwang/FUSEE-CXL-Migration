#!/usr/bin/env bash
# Exp 3: packing decouple via FUSEE_FORCE_THREADS_PER_TYPE.
# Fixed: T=64, N=4, shards=16, routing=worker_id.
# Vary: threads_per_type in {1, 2, 4, 8, 16} (default at T=64 N=4 is 8).
# 1 = max packing (1 thread handles all 16 shards); 16 = no packing (1:1).
# Workloads: xhost_write zipf-0.99 and uniform.
# Hypothesis tests:
#   - If lock contention dominates → thpt ~flat across threads_force
#   - If packing dominates → thpt scales linearly with threads_force
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter17A_exp3_packing_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv
echo "dist,T,N,threads_force,routing,rep,thpt,wp50_us,wp99_us,wc_s" > $CSV

NUM_BUCKETS=8388608
TRANS_OPS=5000000     # back to short trace (sweep, not perf sampling)
DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024
CB=131072
TRACE=/tmp/microbench_traces
T=64; N=4; ROUTING=worker_id

run_cell() {
  local dist=$1 force=$2 rep=$3
  local trace_h0 trace_h1
  if [ "$dist" = zipf ]; then
    trace_h0=$TRACE/bench_xhost_write_zipf-0.99_h0
    trace_h1=$TRACE/bench_xhost_write_zipf-0.99_h1
  else
    trace_h0=$TRACE/bench_xhost_write_uniform_h0
    trace_h1=$TRACE/bench_xhost_write_uniform_h1
  fi
  local cookie=$RANDOM$RANDOM
  local id="${dist}_thr${force}_rep${rep}"
  for h in g3 g4; do
    ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)

  local force_env=""
  if [ "$force" != "default" ]; then
    force_env="FUSEE_FORCE_THREADS_PER_TYPE=$force"
  fi

  timeout 90 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$ROUTING $force_env \
    ./tests/protocol_a_ycsb $DEV \
      ${trace_h0}.spec_load ${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1 &
  timeout 90 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$ROUTING $force_env \
    ./tests/protocol_a_ycsb $DEV \
      ${trace_h1}.spec_load ${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local wp50=$(grep -oP 'w_p50_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local wp99=$(grep -oP 'w_p99_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local wp50_us=$(awk -v n=${wp50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local wp99_us=$(awk -v n=${wp99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  echo "$dist,$T,$N,$force,$ROUTING,$rep,$thpt,$wp50_us,$wp99_us,$wc" >> $CSV
  echo "  dist=$dist threads_force=$force rep=$rep thpt=${thpt} wc=${wc}s"
}

for dist in zipf uniform; do
  for force in 1 2 4 8 16 default; do
    for rep in 1 2 3; do
      run_cell $dist $force $rep
    done
  done
done

echo "=== Medians ==="
awk -F',' 'NR>1 {k=$1"_thr"$4; v[k]=v[k]" "$7} END{
  for(k in v){
    n=split(v[k],a," "); delete vs; m=0
    for(i=1;i<=n;i++) if(a[i]!="") vs[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(vs[i]>vs[j]){t=vs[i];vs[i]=vs[j];vs[j]=t}
    printf "%-20s median=%.3f Mops\n", k, vs[int((m+1)/2)]/1e6
  }
}' $CSV | sort
echo OUT: $OUT
