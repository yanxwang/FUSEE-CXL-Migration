#!/usr/bin/env bash
# iter-18A Phase 2.1 — T-sweep for xhost_read.
# Probe-off 3 reps (thpt baseline) + probe-on 1 rep (full-T decomp).
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase2_T_sweep_$TS
mkdir -p $OUT/raw_off $OUT/raw_on $OUT/probe_h0 $OUT/probe_h1
CSV=$OUT/grid.csv
echo "build,T,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV

NUM_BUCKETS=8388608
TRANS_FULL=5000000
TRANS_PROBE=200000
DEV=/dev/dax0.0
BUILD_OFF=build-cxl-w1-v1024
BUILD_ON=build-cxl-w1-v1024-readprobe
CB=131072
TRACE=/tmp/microbench_traces
WORKLOAD=xhost_read
TBASE=bench_xhost_read_zipf-0.99

Ts=(1 2 4 8 16 32 64)

kill_all() {
  for h in g3 g4; do
    ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
}

run_cell() {
  local build=$1 T=$2 rep=$3 trans_ops=$4 probe_env=$5
  local subdir=raw_off; [[ "$build" = *readprobe* ]] && subdir=raw_on
  local cookie=$RANDOM$RANDOM
  local id="T${T}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h0.spec_load $TRACE/${TBASE}_h0.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h1.spec_load $TRACE/${TBASE}_h1.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local h0f=$OUT/$subdir/${id}_h0.out
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $h0f | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $h0f | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $h0f | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local btag=off; [[ "$build" = *readprobe* ]] && btag=on
  echo "$btag,$T,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  $btag T=$T rep=$rep thpt=$thpt wc=${wc}s"
}

echo "=== Probe-off T-sweep (3 reps) ==="
for T in "${Ts[@]}"; do
  for rep in 1 2 3; do
    run_cell $BUILD_OFF $T $rep $TRANS_FULL ""
  done
done

# Probe-on dumps for decomp
ssh root@g3 'rm -rf /tmp/iter18a_phase2_T' >/dev/null
ssh root@g4 'rm -rf /tmp/iter18a_phase2_T' >/dev/null
echo "=== Probe-on T-sweep (1 rep each, TRANS=$TRANS_PROBE) ==="
for T in "${Ts[@]}"; do
  ssh root@g3 "mkdir -p /tmp/iter18a_phase2_T/h0/T$T" >/dev/null
  ssh root@g4 "mkdir -p /tmp/iter18a_phase2_T/h1/T$T" >/dev/null
  run_cell $BUILD_ON $T 1 $TRANS_PROBE "FUSEE_PROBE_DUMP=/tmp/iter18a_phase2_T/h\$FUSEE_HOST_ID/T$T/probe"
done

echo "=== Pulling probe dumps + analyzing each T ==="
for T in "${Ts[@]}"; do
  mkdir -p $OUT/probe_h0/T$T $OUT/probe_h1/T$T
  rsync -avz root@g3:/tmp/iter18a_phase2_T/h0/T$T/ $OUT/probe_h0/T$T/ >/dev/null 2>&1
  rsync -avz root@g4:/tmp/iter18a_phase2_T/h1/T$T/ $OUT/probe_h1/T$T/ >/dev/null 2>&1
  echo "--- T=$T decomp ---" | tee -a $OUT/decomp.log
  python3 $ROOT/scripts/iter18A_read_decomp_analyze.py \
    $OUT/probe_h0/T$T $OUT/probe_h1/T$T --cpu-ghz 2.4 \
    --out $OUT/decomp_T${T}.csv 2>&1 | tee -a $OUT/decomp.log
done

echo "=== Median thpt summary ==="
awk -F',' 'NR>1 && $4>0 {key=$1"_T"$2; v[key]=v[key]" "$4} END{
  for(k in v){
    n=split(v[k],a," "); delete s; m=0
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-10s median=%.3f Mops\n", k, s[int((m+1)/2)]/1e6
  }
}' $CSV | sort

# Anomaly scan (§13 gate 5): cell < 0.01 Mops OR < neighbor-geomean/10
echo "=== Anomaly scan ===" | tee $OUT/gap_to_target.md
echo "" >> $OUT/gap_to_target.md
echo "## Anomaly scan (§13 gate 5)" >> $OUT/gap_to_target.md
awk -F',' 'NR>1 && $1=="off" && $4>0 {sum[$2]+=$4; cnt[$2]++}
END{for(t in sum) printf "T=%s mean_thpt=%.3f Mops\n",t,sum[t]/cnt[t]/1e6}' $CSV | sort -t'=' -k2,2n
echo "" >> $OUT/gap_to_target.md
echo "OUT: $OUT"
