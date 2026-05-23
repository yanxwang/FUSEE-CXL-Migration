#!/usr/bin/env bash
# iter-18A Phase 2.2 — V-sweep for xhost_read, T=16, zipf-0.99, N=0.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase2_V_sweep_$TS
mkdir -p $OUT/raw_off $OUT/raw_on $OUT/probe_h0 $OUT/probe_h1
CSV=$OUT/grid.csv
echo "build,V,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV
NUM_BUCKETS=8388608; TRANS_FULL=5000000; TRANS_PROBE=200000
DEV=/dev/dax0.0
BUILD_OFF=build-cxl-w1-v1024; BUILD_ON=build-cxl-w1-v1024-readprobe
CB=131072; TRACE=/tmp/microbench_traces
WORKLOAD=xhost_read; TBASE=bench_xhost_read_zipf-0.99
T=16
Vs=(64 256 512 1024)

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 1; }

run_cell() {
  local build=$1 V=$2 rep=$3 trans_ops=$4 probe_env=$5
  local subdir=raw_off; [[ "$build" = *readprobe* ]] && subdir=raw_on
  local cookie=$RANDOM$RANDOM
  local id="V${V}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h0.spec_load $TRACE/${TBASE}_h0.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h1.spec_load $TRACE/${TBASE}_h1.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1); [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $OUT/$subdir/${id}_h0.out | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local btag=off; [[ "$build" = *readprobe* ]] && btag=on
  echo "$btag,$V,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  $btag V=$V rep=$rep thpt=$thpt wc=${wc}s"
}

echo "=== Probe-off V-sweep (3 reps) ==="
for V in "${Vs[@]}"; do
  for rep in 1 2 3; do run_cell $BUILD_OFF $V $rep $TRANS_FULL ""; done
done

ssh root@g3 'rm -rf /tmp/iter18a_phase2_V' >/dev/null; ssh root@g4 'rm -rf /tmp/iter18a_phase2_V' >/dev/null
echo "=== Probe-on V-sweep (1 rep each) ==="
for V in "${Vs[@]}"; do
  ssh root@g3 "mkdir -p /tmp/iter18a_phase2_V/h0/V$V" >/dev/null
  ssh root@g4 "mkdir -p /tmp/iter18a_phase2_V/h1/V$V" >/dev/null
  run_cell $BUILD_ON $V 1 $TRANS_PROBE "FUSEE_PROBE_DUMP=/tmp/iter18a_phase2_V/h\$FUSEE_HOST_ID/V$V/probe"
done

echo "=== Pulling + analyzing ==="
for V in "${Vs[@]}"; do
  mkdir -p $OUT/probe_h0/V$V $OUT/probe_h1/V$V
  rsync -avz root@g3:/tmp/iter18a_phase2_V/h0/V$V/ $OUT/probe_h0/V$V/ >/dev/null 2>&1
  rsync -avz root@g4:/tmp/iter18a_phase2_V/h1/V$V/ $OUT/probe_h1/V$V/ >/dev/null 2>&1
  echo "--- V=$V ---" | tee -a $OUT/decomp.log
  python3 $ROOT/scripts/iter18A_read_decomp_analyze.py \
    $OUT/probe_h0/V$V $OUT/probe_h1/V$V --cpu-ghz 2.4 \
    --out $OUT/decomp_V${V}.csv 2>&1 | tee -a $OUT/decomp.log
done

echo "=== Medians ==="
awk -F',' 'NR>1 && $4>0 {key=$1"_V"$2; v[key]=v[key]" "$4} END{
  for(k in v){
    n=split(v[k],a," "); delete s; m=0
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-10s median=%.3f Mops\n",k,s[int((m+1)/2)]/1e6
  }
}' $CSV | sort
echo "OUT: $OUT"
