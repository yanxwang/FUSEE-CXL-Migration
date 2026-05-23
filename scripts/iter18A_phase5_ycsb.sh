#!/usr/bin/env bash
# iter-18A Phase 5 — YCSB workloadc sanity.
# T ∈ {16, 32, 64}, N ∈ {0, 4}, 3 reps. Workloadc is 100% read on zipf.
# Goal: validate Phase 4 3D ReadStagingMatrix fix on YCSB (vs iter-17A
# workloadc T=16 N=4 = 0.016 Mops). Require N=4 ≥ 50% N=0 baseline.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase5_ycsb_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv
echo "T,N,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV

NUM_BUCKETS=8388608; TRANS=5000000; DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024; CB=131072
WORKLOAD_DIR=/root/FUSEE_CXL/setup/workloads
WL=workloadc

Ts=(16 32 64); Ns=(0 4)

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 1; }

run_cell() {
  local T=$1 N=$2 rep=$3
  local cookie=$RANDOM$RANDOM
  local id="T${T}_N${N}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 180 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$WL FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $WORKLOAD_DIR/${WL}.spec_load $WORKLOAD_DIR/${WL}.spec_trans \
      $NUM_BUCKETS $TRANS 2>&1" > $OUT/raw/${id}_h0.out 2>&1 &
  timeout 180 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$WL FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $WORKLOAD_DIR/${WL}.spec_load $WORKLOAD_DIR/${WL}.spec_trans \
      $NUM_BUCKETS $TRANS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1); [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  echo "$T,$N,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  T=$T N=$N rep=$rep thpt=$thpt wc=${wc}s"
}

for T in "${Ts[@]}"; do
  for N in "${Ns[@]}"; do
    for rep in 1 2 3; do run_cell $T $N $rep; done
  done
done

echo "=== Medians ==="
awk -F',' 'NR>1 && $4>0 {k="T"$1"_N"$2; v[k]=v[k]" "$4} END{
  for(k in v){n=split(v[k],a," "); delete s; m=0;
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-14s median=%.3f Mops\n",k,s[int((m+1)/2)]/1e6
  }
}' $CSV | sort

echo "OUT: $OUT"
