#!/usr/bin/env bash
# iter-18A Phase 4 — 7-group × 2-dist sweep for xhost_read.
# Mirrors iter-17A xhost_write 7-group uniform sweep grid:
#   T ∈ {1,2,4,8,16,32,64}, N ∈ {0,4,8}, routing ∈ {worker_id, key_hash}
#   × dist ∈ {uniform, zipf-0.99}, 3 reps each = 252 runs.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase4_7group_sweep_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv
echo "dist,T,N,routing,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV

NUM_BUCKETS=8388608; TRANS=5000000; DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024; CB=131072; TRACE=/tmp/microbench_traces

Ts=(1 2 4 8 16 32 64)
Ns=(0 4 8)
ROUTINGS=(worker_id key_hash)
DISTS=(uniform zipf-0.99)

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 1; }

run_cell() {
  local dist=$1 T=$2 N=$3 routing=$4 rep=$5
  local cookie=$RANDOM$RANDOM
  local id="d${dist}_T${T}_N${N}_${routing}_rep${rep}"
  local tbase="bench_xhost_read_${dist}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$routing \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${tbase}_h0.spec_load $TRACE/${tbase}_h0.spec_trans \
      $NUM_BUCKETS $TRANS 2>&1" > $OUT/raw/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$routing \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${tbase}_h1.spec_load $TRACE/${tbase}_h1.spec_trans \
      $NUM_BUCKETS $TRANS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1); [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  echo "$dist,$T,$N,$routing,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  d=$dist T=$T N=$N $routing rep=$rep thpt=$thpt wc=${wc}s"
}

total=0; for d in "${DISTS[@]}"; do for T in "${Ts[@]}"; do for N in "${Ns[@]}"; do for r in "${ROUTINGS[@]}"; do for rep in 1 2 3; do total=$((total+1)); done; done; done; done; done
echo "Total cells: $total"

i=0
for dist in "${DISTS[@]}"; do
  for T in "${Ts[@]}"; do
    for N in "${Ns[@]}"; do
      for routing in "${ROUTINGS[@]}"; do
        for rep in 1 2 3; do
          i=$((i+1))
          echo "[$i/$total]"
          run_cell $dist $T $N $routing $rep
        done
      done
    done
  done
done

echo "=== Medians ==="
awk -F',' 'NR>1 && $6>0 {k=$1"_T"$2"_N"$3"_"$4; v[k]=v[k]" "$6} END{
  for(k in v){n=split(v[k],a," "); delete s; m=0;
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-32s median=%.3f Mops\n",k,s[int((m+1)/2)]/1e6
  }
}' $CSV | sort | head -50
echo "OUT: $OUT"
