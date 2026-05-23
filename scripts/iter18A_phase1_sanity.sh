#!/usr/bin/env bash
# iter-18A Phase 1.5: sanity test for xhost_read RDTSCP+LFENCE probe framework.
# (a) Probe-off vs probe-on thpt compare at T ∈ {1, 8, 64} (3 reps probe-off,
#     1 rep probe-on with shorter trace to fit per-thread 512MB probe ring).
# (b) Pull probe files back; run analyzer; verify ∑stage ≈ wall-clock + dump
#     per-stage p50_ns breakdown for user review.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase1_sanity_$TS
mkdir -p $OUT/raw_off $OUT/raw_on $OUT/probe_h0 $OUT/probe_h1
CSV=$OUT/thpt_compare.csv
echo "build,T,rep,thpt,wp50_us,wp99_us,wc_s" > $CSV

NUM_BUCKETS=8388608
TRANS_FULL=5000000      # probe-off full trace
TRANS_PROBE=200000      # probe-on shorter trace (fits 512MB per-thread)
DEV=/dev/dax0.0
BUILD_OFF=build-cxl-w1-v1024
BUILD_ON=build-cxl-w1-v1024-readprobe
CB=131072
TRACE=/tmp/microbench_traces
WORKLOAD=xhost_read
TRACE_BASE=bench_xhost_read_zipf-0.99
DIST=zipf-0.99

# Kill any leftover binary
kill_all() {
  for h in g3 g4; do
    ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
}

run_cell() {
  local build=$1 T=$2 rep=$3 trans_ops=$4 probe_env=$5
  local cookie=$RANDOM$RANDOM
  local label="${build}_T${T}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TRACE_BASE}_h0.spec_load $TRACE/${TRACE_BASE}_h0.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/raw_${build/build-cxl-w1-v1024/raw}/${label}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TRACE_BASE}_h1.spec_load $TRACE/${TRACE_BASE}_h1.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/raw_${build/build-cxl-w1-v1024/raw}/${label}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  # Find the actual file
  local h0f=$OUT/raw_${build/build-cxl-w1-v1024/raw}/${label}_h0.out
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $h0f | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local wp50=$(grep -oP 'r_p50_ns=\K[0-9]+' $h0f | head -1)
  local wp99=$(grep -oP 'r_p99_ns=\K[0-9]+' $h0f | head -1)
  local wp50_us=$(awk -v n=${wp50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local wp99_us=$(awk -v n=${wp99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local btag=off; [[ "$build" = *readprobe* ]] && btag=on
  echo "$btag,$T,$rep,$thpt,$wp50_us,$wp99_us,$wc" >> $CSV
  echo "  $btag T=$T rep=$rep thpt=$thpt wc=${wc}s"
}

# Cleanup output sub-dirs
rm -rf $OUT/raw $OUT/raw_-readprobe 2>/dev/null
mkdir -p $OUT/raw_off $OUT/raw_on

# Override run_cell to write to fixed subdirs
run_cell() {
  local build=$1 T=$2 rep=$3 trans_ops=$4 probe_env=$5
  local cookie=$RANDOM$RANDOM
  local subdir=raw_off
  [[ "$build" = *readprobe* ]] && subdir=raw_on
  local label="${subdir/raw_/b}_T${T}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TRACE_BASE}_h0.spec_load $TRACE/${TRACE_BASE}_h0.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${label}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$build && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id $probe_env \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TRACE_BASE}_h1.spec_load $TRACE/${TRACE_BASE}_h1.spec_trans \
      $NUM_BUCKETS $trans_ops 2>&1" > $OUT/$subdir/${label}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local h0f=$OUT/$subdir/${label}_h0.out
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $h0f | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local wp50=$(grep -oP 'r_p50_ns=\K[0-9]+' $h0f | head -1)
  local wp99=$(grep -oP 'r_p99_ns=\K[0-9]+' $h0f | head -1)
  local wp50_us=$(awk -v n=${wp50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local wp99_us=$(awk -v n=${wp99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local btag=off; [[ "$build" = *readprobe* ]] && btag=on
  echo "$btag,$T,$rep,$thpt,$wp50_us,$wp99_us,$wc" >> $CSV
  echo "  $btag T=$T rep=$rep thpt=$thpt p50=${wp50_us}us wc=${wc}s"
}

# --- (a) probe-off thpt baseline ---
echo "=== Probe-off thpt baseline (3 reps each) ==="
for T in 1 8 64; do
  for rep in 1 2 3; do
    run_cell $BUILD_OFF $T $rep $TRANS_FULL ""
  done
done

# --- (b) probe-on single rep w/ FUSEE_PROBE_DUMP ---
echo "=== Probe-on (1 rep each, TRANS_OPS=$TRANS_PROBE, dumps to /tmp/iter18a_probe/h*/) ==="
# Make probe dump dirs on hosts; clear any prior probes
ssh root@g3 'rm -rf /tmp/iter18a_probe && mkdir -p /tmp/iter18a_probe/h0' 2>&1 >/dev/null
ssh root@g4 'rm -rf /tmp/iter18a_probe && mkdir -p /tmp/iter18a_probe/h1' 2>&1 >/dev/null
for T in 1 8 64; do
  # Clear probe dir between T runs
  ssh root@g3 "rm -rf /tmp/iter18a_probe/h0/T$T && mkdir -p /tmp/iter18a_probe/h0/T$T" >/dev/null
  ssh root@g4 "rm -rf /tmp/iter18a_probe/h1/T$T && mkdir -p /tmp/iter18a_probe/h1/T$T" >/dev/null
  run_cell $BUILD_ON $T 1 $TRANS_PROBE "FUSEE_PROBE_DUMP=/tmp/iter18a_probe/h\$FUSEE_HOST_ID/T$T/probe"
done

# --- (c) Pull probe files back ---
echo "=== Pulling probe files for analysis ==="
for T in 1 8 64; do
  mkdir -p $OUT/probe_h0/T$T $OUT/probe_h1/T$T
  rsync -avz root@g3:/tmp/iter18a_probe/h0/T$T/ $OUT/probe_h0/T$T/ >/dev/null 2>&1
  rsync -avz root@g4:/tmp/iter18a_probe/h1/T$T/ $OUT/probe_h1/T$T/ >/dev/null 2>&1
  echo "  T=$T h0 files=$(ls $OUT/probe_h0/T$T/ 2>/dev/null | wc -l), h1 files=$(ls $OUT/probe_h1/T$T/ 2>/dev/null | wc -l)"
done

# --- (d) Run analyzer for each T ---
echo "=== Stage decomp per T ==="
for T in 1 8 64; do
  echo "--- T=$T ---" | tee -a $OUT/decomp.log
  python3 $ROOT/scripts/iter18A_read_decomp_analyze.py \
    $OUT/probe_h0/T$T $OUT/probe_h1/T$T --cpu-ghz 2.4 \
    --out $OUT/decomp_T${T}.csv 2>&1 | tee -a $OUT/decomp.log
  echo "" | tee -a $OUT/decomp.log
done

# --- (e) Summary ---
echo "=== Probe-off vs probe-on median thpt ==="
awk -F',' 'NR>1 && $4>0 {key=$1"_T"$2; v[key]=v[key]" "$4} END{
  for(k in v){
    n=split(v[k],a," "); delete s; m=0
    for(i=1;i<=n;i++) if(a[i]!="") s[++m]=a[i]+0
    for(i=1;i<m;i++) for(j=i+1;j<=m;j++) if(s[i]>s[j]){t=s[i];s[i]=s[j];s[j]=t}
    printf "%-12s median=%.3f Mops\n", k, s[int((m+1)/2)]/1e6
  }
}' $CSV | sort

echo "OUT: $OUT"
