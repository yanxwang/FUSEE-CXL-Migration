#!/usr/bin/env bash
# iter-19A Phase 1 Anomaly A — Experiment 1
# Does B-H3 fix (B_no_flush) eliminate Anomaly A?
# Compares cache_pct sweep on baseline vs B_no_flush builds.
#
# Grid: 2 builds × 7 cache_pct × 3 reps = 42 cells
#   builds: baseline (lrprobe), B_no_flush
#   cache_pct: 1, 2, 5, 10, 20, 50, 100
#   dist: zipf-0.99, T=64, V=1024

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase1_anomaly_a_exp1_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

declare -A CACHE_BUCKETS=(
  [1]=16384  [2]=32768  [5]=65536  [10]=131072
  [20]=262144 [50]=524288 [100]=2097152
)

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
TRACE=/tmp/microbench_traces
V=1024
T=64

echo "build,V,T,cache_pct,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,r2hit,r2miss_local,wallclock_s" > $CSV

extract_metric() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}
extract_path_agg() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep "after_TRANS AGG" "$file" 2>/dev/null | head -1 | grep -oP "$key=\K[\d.]+" | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}
sum_path_agg() {
  local f0=$1 f1=$2 key=$3
  local v0=$(extract_path_agg "$f0" "$key" 0)
  local v1=$(extract_path_agg "$f1" "$key" 0)
  awk -v a=$v0 -v b=$v1 'BEGIN{print a+b}'
}

map_build() {
  case $1 in
    baseline) echo "build-cxl-w1-v1024-lrprobe" ;;
    *)        echo "build-cxl-w1-v1024-$1" ;;
  esac
}

run_cell() {
  local build_label=$1 cache_pct=$2 rep=$3
  local cb=${CACHE_BUCKETS[$cache_pct]}
  local builddir=$(map_build $build_label)
  local id="${build_label}_c${cache_pct}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_zipf-0.99_h0
  local trace_h1=bench_local_read_zipf-0.99_h1

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)
  timeout 180 ssh $H0 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 180 ssh $H1 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h1}.spec_load $TRACE/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  pid1=$!
  wait $pid0; wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract_metric $out_h0 'r_p99_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r2hit=$(sum_path_agg $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path_agg $out_h0 $out_h1 'r2miss_local')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$build_label,$V,$T,$cache_pct,$cb,local_read,zipf-0.99,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r2hit,$r2miss,$wc" >> $CSV
  echo "  $build_label/c${cache_pct} rep=$rep thpt=$thpt_mops Mops r_p50=${r_p50_us}us"
}

echo "==== Anomaly A Exp 1: B-H3 fix interaction with cache_pct ===="
for build in baseline B_no_flush; do
  echo "--- $build ---"
  for cache_pct in 1 2 5 10 20 50 100; do
    for rep in 1 2 3; do
      run_cell $build $cache_pct $rep
    done
  done
done

echo ""
echo "==== Analysis ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print()
print(f"{'cache%':>7}{'baseline':>11}{'B_no_flush':>13}{'gain%':>10}{'gap':>8}")
for cp in ["1","2","5","10","20","50","100"]:
    b_thpt = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="baseline" and r["cache_pct"]==cp] or [0])
    nf_thpt = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="B_no_flush" and r["cache_pct"]==cp] or [0])
    gain = (nf_thpt - b_thpt) / b_thpt * 100 if b_thpt > 0 else 0
    print(f"{cp:>7}{b_thpt:>11.2f}{nf_thpt:>13.2f}{gain:>9.1f}%{nf_thpt-b_thpt:>8.2f}")

print()
b1 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="baseline" and r["cache_pct"]=="1"] or [0])
b100 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="baseline" and r["cache_pct"]=="100"] or [0])
nf1 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="B_no_flush" and r["cache_pct"]=="1"] or [0])
nf100 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="B_no_flush" and r["cache_pct"]=="100"] or [0])

print(f"=== Anomaly A magnitude ===")
print(f"baseline    c100/c1 = {b100/b1:.3f}  (current Anomaly A: 0.39 collapse)")
print(f"B_no_flush  c100/c1 = {nf100/nf1:.3f}")
print()
if abs(nf100/nf1 - b100/b1) < 0.05:
    print(f"→ Anomaly A is INDEPENDENT of flush. Need different RCA (probably cache_pool DRAM access cost).")
    print(f"  Next experiment: isolated TRANS-phase PMU (need TRANS_OPS↑ or perf timestamp filter)")
elif nf100/nf1 > 0.7:
    print(f"→ B_no_flush LARGELY ELIMINATES Anomaly A — flush storm was masking real mechanism.")
    print(f"  Ship B-H3 fix solves both anomalies.")
else:
    print(f"→ B_no_flush PARTIALLY closes Anomaly A. Both flush AND cache_pool DRAM contribute.")
    print(f"  Both fixes are needed.")
PY
echo "OUT: $OUT"
