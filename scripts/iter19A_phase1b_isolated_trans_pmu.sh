#!/usr/bin/env bash
# iter-19A Phase 1b — Anomaly A residual cause via isolated-TRANS PMU
#
# Goal: confirm/refute LLC capacity miss hypothesis for the residual
# ~11% Anomaly A signal at cache_pct=100 (after B-H3 fix). At cache_pct=1
# cache_pool is ~71 MB (fits L3), at cache_pct=100 it's ~9 GB (DRAM).
#
# Setup: TRANS_OPS=50M makes TRANS take ~ 100-300s, dominating wallclock
# (~85%). perf stat -D 25000 attaches 25s after binary start, skipping
# most of LOAD (~15-20s on g1/g2). The PMU counts are TRANS-dominated.
#
# Grid: 2 builds × 3 cache_pct × 3 reps = 18 cells
# Builds:
#   - lrprobe   (baseline, B-H3 still active = flush storm)
#   - bnoflush  (B-H3 removed = isolate Anomaly A residual)
# cache_pct: 1, 10, 100 (= 16384, 131072, 2097152 buckets)
# T = 64

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase1b_trans_pmu_$TS
mkdir -p $OUT/raw $OUT/perf
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=50000000
TRACE=/tmp/microbench_traces
V=1024
T=64
PERF_DELAY_MS=25000

declare -A CACHE_BUCKETS=( [1]=16384 [10]=131072 [100]=2097152 )

PMU_EVENTS='cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,cache-references,cache-misses,branches,branch-misses'

echo "build,cache_pct,cache_buckets,rep,thpt_Mops,r_p50_us,r_p99_us,r2hit,r2miss,trans_wall_s,wallclock_s,IPC,L1_miss_rate,LLC_miss_rate,LLC_miss_per_inst" > $CSV

extract() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}
extract_path_agg() {
  local file=$1 key=$2
  local v=$(grep "after_TRANS AGG" "$file" 2>/dev/null | head -1 | grep -oP "$key=\K[\d.]+" | head -1)
  [ -n "$v" ] && echo "$v" || echo "0"
}
sum_path() {
  awk -v a=$(extract_path_agg "$1" "$3") -v b=$(extract_path_agg "$2" "$3") 'BEGIN{print a+b}'
}

# Parse perf stat output. perf stat with -x outputs CSV-ish:
#   <count>;<unit>;<event>;<run_time>;<pct>;...
parse_perf() {
  local pfile=$1 evt=$2
  grep -E "[;,]${evt}[;,]" "$pfile" 2>/dev/null | head -1 | awk -F';' '{print $1}' | tr -d ',' | tr -d '<not counted>'
}

run_cell() {
  local build_label=$1 cache_pct=$2 rep=$3
  local cb=${CACHE_BUCKETS[$cache_pct]}
  local builddir="build-cxl-w1-v1024-$build_label"
  local id="${build_label}_c${cache_pct}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local perf_h0=$OUT/perf/${id}_rep${rep}_h0.txt
  local perf_h1=$OUT/perf/${id}_rep${rep}_h1.txt
  local trace_h0=bench_local_read_zipf-0.99_h0
  local trace_h1=bench_local_read_zipf-0.99_h1

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)

  # Wrap binary in perf stat with delayed attach. Use --per-process so we
  # capture full binary including forked children. -A reports per-CPU
  # aggregate; we want one summary per process.
  # --delay <ms> = delay event counting (not process start).
  timeout 600 ssh $H0 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read FUSEE_KV_SIZE=$V \
    FUSEE_CACHE_BUCKETS=$cb \
    perf stat -e $PMU_EVENTS -x ';' -D $PERF_DELAY_MS \
      -o /tmp/perf_out_${cookie}.txt -- \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1; \
    cat /tmp/perf_out_${cookie}.txt 2>&1; rm -f /tmp/perf_out_${cookie}.txt" > $out_h0 2>&1 &
  local pid0=$!
  timeout 600 ssh $H1 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read FUSEE_KV_SIZE=$V \
    FUSEE_CACHE_BUCKETS=$cb \
    perf stat -e $PMU_EVENTS -x ';' -D $PERF_DELAY_MS \
      -o /tmp/perf_out_${cookie}.txt -- \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h1}.spec_load $TRACE/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1; \
    cat /tmp/perf_out_${cookie}.txt 2>&1; rm -f /tmp/perf_out_${cookie}.txt" > $out_h1 2>&1 &
  local pid1=$!
  wait $pid0; wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  # Split: YCSB stdout + perf-csv tail
  awk '/^# started on/{started=1} started{print > "/tmp/perf_split.txt"; next} {print}' \
    < $out_h0 > /tmp/ycsb_h0.txt 2>/dev/null
  cp /tmp/perf_split.txt $perf_h0 2>/dev/null

  local thpt=$(extract $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract $out_h0 'r_p99_ns' 0)
  local trans_wall=$(extract $out_h0 'trans_wall_max' 0)
  local r2hit=$(sum_path $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path $out_h0 $out_h1 'r2miss_local')
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.2f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.2f", n/1000.0}')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  # Parse perf from h0 file (last block after '# started on')
  local cycles=$(grep -oP "^\d+;[^;]*;cycles" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  local insns=$(grep -oP "^\d+;[^;]*;instructions" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  local l1l=$(grep -oP "^\d+;[^;]*;L1-dcache-loads" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  local l1m=$(grep -oP "^\d+;[^;]*;L1-dcache-load-misses" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  local llcl=$(grep -oP "^\d+;[^;]*;LLC-loads" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  local llcm=$(grep -oP "^\d+;[^;]*;LLC-load-misses" $out_h0 2>/dev/null | head -1 | awk -F';' '{print $1}')
  cycles=${cycles:-0}; insns=${insns:-0}; l1l=${l1l:-0}; l1m=${l1m:-0}; llcl=${llcl:-0}; llcm=${llcm:-0}
  local ipc=$(awk -v i=$insns -v c=$cycles 'BEGIN{print (c>0)?i/c:0}')
  local l1_miss=$(awk -v m=$l1m -v l=$l1l 'BEGIN{print (l>0)?m/l*100:0}')
  local llc_miss=$(awk -v m=$llcm -v l=$llcl 'BEGIN{print (l>0)?m/l*100:0}')
  local llc_per_inst=$(awk -v m=$llcm -v i=$insns 'BEGIN{print (i>0)?m/i*1000:0}')

  echo "$build_label,$cache_pct,$cb,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r2hit,$r2miss,$trans_wall,$wc,$ipc,$l1_miss,$llc_miss,$llc_per_inst" >> $CSV
  printf "  %s/c%s rep=%s thpt=%s p50=%s us trans=%ss IPC=%.2f L1m=%.1f%% LLCm=%.1f%% LLC/Ki=%.2f\n" \
    "$build_label" "$cache_pct" "$rep" "$thpt_mops" "$r_p50_us" "$trans_wall" \
    "$ipc" "$l1_miss" "$llc_miss" "$llc_per_inst"
}

echo "==== iter-19A Phase 1b: isolated-TRANS PMU sweep ===="
echo "  TRANS_OPS=$TRANS_OPS  perf_delay=${PERF_DELAY_MS}ms  T=$T"
for build in lrprobe bnoflush; do
  echo "--- build $build ---"
  for cp in 1 10 100; do
    for rep in 1 2 3; do
      run_cell $build $cp $rep
    done
  done
done

echo ""
echo "==== ANALYSIS ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
def med(sel, key): return statistics.median([float(r[key]) for r in sel]) if sel else 0

print(f"{'build':<10}{'cache%':>8}{'thpt Mops':>11}{'IPC':>7}{'L1 miss%':>10}{'LLC miss%':>11}{'LLC/Kins':>10}")
for b in ["lrprobe","bnoflush"]:
  for cp in ["1","10","100"]:
    sel=[r for r in rows if r["build"]==b and r["cache_pct"]==cp]
    if not sel: continue
    print(f"{b:<10}{cp:>8}{med(sel,'thpt_Mops'):>11.2f}{med(sel,'IPC'):>7.2f}{med(sel,'L1_miss_rate'):>10.2f}{med(sel,'LLC_miss_rate'):>11.2f}{med(sel,'LLC_miss_per_inst'):>10.3f}")

print()
print("=== Anomaly A magnitude per build ===")
for b in ["lrprobe","bnoflush"]:
  t1=med([r for r in rows if r["build"]==b and r["cache_pct"]=="1"],'thpt_Mops')
  t100=med([r for r in rows if r["build"]==b and r["cache_pct"]=="100"],'thpt_Mops')
  if t1>0:
    print(f"  {b}: c100/c1 = {t100/t1:.3f}  (Anomaly A magnitude)")

print()
print("=== LLC miss rate growth with cache_pct (bnoflush, B-H3 removed) ===")
for cp in ["1","10","100"]:
  sel=[r for r in rows if r["build"]=="bnoflush" and r["cache_pct"]==cp]
  if sel:
    llc=med(sel,'LLC_miss_rate'); per=med(sel,'LLC_miss_per_inst')
    print(f"  cache_pct={cp}: LLC miss = {llc:.1f}%  ({per:.3f} per K-inst)")

print()
print("Verdict guidance:")
print("- LLC miss rate rises sharply c1 -> c100 in bnoflush -> LLC pressure CONFIRMED")
print("- LLC miss rate flat -> LLC NOT the cause; check IPC/branch-miss / other counter")
PY
echo "OUT: $OUT"
