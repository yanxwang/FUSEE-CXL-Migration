#!/usr/bin/env bash
# Smoke test after g1/g2 switch port remap to 4×Micron 128GB modules (512 GB total)
# Compare throughput to:
#   - iter-18A g3/g4 historical (full BW): A=1.181 B=4.066 C=5.203 D=1.108 Mops
#   - g1/g2 pre-remap (2×Samsung, G5x8 switch port): A=0.63 B=2.28 C=2.78 D=0.58 Mops
# Hypothesis: switch DSP port is G5x8 HW limited; module count behind switch
# shouldn't matter. BW probe confirms 26.4 GB/s (unchanged from pre-remap).
# This smoke verifies the YCSB-level numbers.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_remap_smoke_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024
NUM_BUCKETS=8388608
TRANS_OPS=5000000
TRACE=/tmp/microbench_traces
V=1024

echo "test,T,N,rep,thpt,thpt_Mops,r_p50_us,r_p99_us,wc_s" > $CSV

extract() {
  local file=$1 key=$2
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "0"
}

run_cell() {
  local label=$1 T=$2 N=$3 rep=$4
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${label}_T${T}_N${N}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${label}_T${T}_N${N}_rep${rep}_h1.out
  local trace=bench_xhost_read_zipf-0.99

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)
  timeout 200 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RING_SHARDS_FACTOR=$N \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=$V \
    FUSEE_CACHE_BUCKETS=131072 \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace}_h0.spec_load $TRACE/${trace}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  local pid0=$!
  timeout 200 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RING_SHARDS_FACTOR=$N \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=$V \
    FUSEE_CACHE_BUCKETS=131072 \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace}_h1.spec_load $TRACE/${trace}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  local pid1=$!
  wait $pid0; wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt=$(extract $out_h0 'trans_agg_thpt')
  local r_p50_ns=$(extract $out_h0 'r_p50_ns')
  local r_p99_ns=$(extract $out_h0 'r_p99_ns')
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.2f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.2f", n/1000.0}')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$label,$T,$N,$rep,$thpt,$thpt_mops,$r_p50_us,$r_p99_us,$wc" >> $CSV
  echo "  $label T=$T N=$N rep=$rep thpt=$thpt_mops Mops p50=${r_p50_us}us wc=${wc}s"
}

echo "==== Smoke after switch remap (4×Micron, dax0.0=512 GiB) ===="
for rep in 1 2 3; do
  echo "--- rep $rep ---"
  run_cell A 32 0 $rep
  run_cell B 16 4 $rep
  run_cell C 64 4 $rep
  run_cell D 8  0 $rep
done

echo ""
echo "==== ANALYSIS ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
hist = {"A": 1.181, "B": 4.066, "C": 5.203, "D": 1.108}
pre  = {"A": 0.634, "B": 2.278, "C": 2.777, "D": 0.578}
print(f"{'cell':>5}{'now Mops':>12}{'g34 hist':>12}{'pre-remap':>12}{'vs hist':>10}{'vs pre':>10}")
for cell in ["A","B","C","D"]:
    sel = [float(r["thpt_Mops"]) for r in rows if r["test"]==cell]
    if not sel: continue
    med = statistics.median(sel)
    print(f"{cell:>5}{med:>12.3f}{hist[cell]:>12.3f}{pre[cell]:>12.3f}{med/hist[cell]*100:>9.1f}%{med/pre[cell]*100:>9.1f}%")
PY
echo "OUT: $OUT"
