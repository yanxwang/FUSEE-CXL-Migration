#!/usr/bin/env bash
# iter-18A Phase 2.3 — dist-sweep for xhost_read, T=64, V=1024, N=0.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase2_dist_sweep_$TS
mkdir -p $OUT/raw_off $OUT/raw_on $OUT/probe_h0 $OUT/probe_h1
CSV=$OUT/grid.csv
echo "build,dist,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV
NUM_BUCKETS=8388608; TRANS_FULL=5000000; TRANS_PROBE=200000
DEV=/dev/dax0.0
BUILD_OFF=build-cxl-w1-v1024; BUILD_ON=build-cxl-w1-v1024-readprobe
CB=131072; TRACE=/tmp/microbench_traces
WORKLOAD=xhost_read
T=64
DISTS=(uniform zipf-0.5 zipf-0.99 zipf-1.5)

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 1; }

run_cell() {
  local build=$1 dist=$2 rep=$3 trans_ops=$4 probe_env=$5
  local subdir=raw_off; [[ "$build" = *readprobe* ]] && subdir=raw_on
  local cookie=$RANDOM$RANDOM
  local id="d${dist}_rep${rep}"
  local tbase="bench_xhost_read_${dist}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${tbase}_h0.spec_load $TRACE/${tbase}_h0.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${tbase}_h1.spec_load $TRACE/${tbase}_h1.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1); [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local btag=off; [[ "$build" = *readprobe* ]] && btag=on
  echo "$btag,$dist,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  $btag dist=$dist rep=$rep thpt=$thpt wc=${wc}s"
}

echo "=== Probe-off dist-sweep (3 reps) ==="
for d in "${DISTS[@]}"; do
  for rep in 1 2 3; do run_cell $BUILD_OFF $d $rep $TRANS_FULL ""; done
done

ssh root@g3 'rm -rf /tmp/iter18a_phase2_D' >/dev/null; ssh root@g4 'rm -rf /tmp/iter18a_phase2_D' >/dev/null
echo "=== Probe-on dist-sweep ==="
for d in "${DISTS[@]}"; do
  ssh root@g3 "mkdir -p /tmp/iter18a_phase2_D/h0/$d" >/dev/null
  ssh root@g4 "mkdir -p /tmp/iter18a_phase2_D/h1/$d" >/dev/null
  run_cell $BUILD_ON $d 1 $TRANS_PROBE "FUSEE_PROBE_DUMP=/tmp/iter18a_phase2_D/h\$FUSEE_HOST_ID/$d/probe"
done

echo "=== Pulling + analyzing ==="
for d in "${DISTS[@]}"; do
  mkdir -p $OUT/probe_h0/$d $OUT/probe_h1/$d
  rsync -avz root@g3:/tmp/iter18a_phase2_D/h0/$d/ $OUT/probe_h0/$d/ >/dev/null 2>&1
  rsync -avz root@g4:/tmp/iter18a_phase2_D/h1/$d/ $OUT/probe_h1/$d/ >/dev/null 2>&1
  echo "--- dist=$d ---" | tee -a $OUT/decomp.log
  python3 $ROOT/scripts/iter18A_read_decomp_analyze.py \
    $OUT/probe_h0/$d $OUT/probe_h1/$d --cpu-ghz 2.4 \
    --out $OUT/decomp_${d}.csv 2>&1 | tee -a $OUT/decomp.log
done

echo "=== Medians ==="
awk -F',' 'NR>1 && $4>0 {key=$1"_"$2; v[key]=v[key]" "$4} END{
  for(k in v){
    n=split(v[k],a," "); delete s; m=0
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-22s median=%.3f Mops\n",k,s[int((m+1)/2)]/1e6
  }
}' $CSV | sort
echo "OUT: $OUT"
