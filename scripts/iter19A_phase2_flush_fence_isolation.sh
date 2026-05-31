#!/usr/bin/env bash
# iter-19A Phase 2 — flush vs mfence mechanism isolation
#
# Exp 1: 4 builds × 4 dist × 3 reps × T=64 = 48 cells
#   - baseline       : flush + flush + mfence kept
#   - B_no_flush     : all 3 removed (reference fix)
#   - B1_no_flush_only: only 2 flushes removed, mfence kept
#   - B2_no_fence_only: only mfence removed, flushes kept
#
# Exp 2: T scaling on baseline + B_no_flush, T={8,16,32,64} × zipf-1.5 × 3 reps = 24 cells

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase2_flush_fence_isolation_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
TRACE=/tmp/microbench_traces
V=1024

echo "exp,build,V,T,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,r2hit,r2miss_local,wallclock_s" > $CSV

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
  local exp=$1 build_label=$2 T=$3 keydist=$4 rep=$5
  local builddir=$(map_build $build_label)
  local id="${exp}_${build_label}_T${T}_${keydist}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_${keydist}_h0
  local trace_h1=bench_local_read_${keydist}_h1
  local cb=131072

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
  wait $pid0
  wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract_metric $out_h0 'r_p99_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r2hit=$(sum_path_agg $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path_agg $out_h0 $out_h1 'r2miss_local')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$exp,$build_label,$V,$T,local_read,$keydist,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r2hit,$r2miss,$wc" >> $CSV
  echo "  $exp/$build_label/T${T}/${keydist} rep=$rep thpt=$thpt_mops Mops wc=${wc}s"
}

echo "==== Exp 1: flush vs mfence isolation (T=64, 4 builds × 4 dist × 3 reps) ===="
for build in baseline B_no_flush B1_no_flush_only B2_no_fence_only; do
  echo "--- build $build ---"
  for dist in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
    for rep in 1 2 3; do
      run_cell exp1 $build 64 $dist $rep
    done
  done
done

echo ""
echo "==== Exp 2: T-scaling baseline vs B_no_flush at zipf-1.5 ===="
for build in baseline B_no_flush; do
  echo "--- build $build ---"
  for T in 8 16 32 64; do
    for rep in 1 2 3; do
      run_cell exp2 $build $T zipf-1.5 $rep
    done
  done
done

echo ""
echo "==== ANALYSIS ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))

print("=== Exp 1: 4 builds × 4 dist at T=64 (thpt Mops median) ===")
builds = ["baseline", "B_no_flush", "B1_no_flush_only", "B2_no_fence_only"]
dists = ["uniform", "zipf-0.5", "zipf-0.99", "zipf-1.5"]
print(f"{'build':<22}" + "".join(f"{d:>12}" for d in dists))
for b in builds:
    row = f"{b:<22}"
    for d in dists:
        sel = [float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]==b and r["keydist"]==d]
        if sel:
            row += f"{statistics.median(sel):>12.2f}"
        else:
            row += f"{'--':>12}"
    print(row)

print()
print("=== zipf-1.5 verdict ===")
base = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]=="baseline" and r["keydist"]=="zipf-1.5"])
for b in ["B1_no_flush_only", "B2_no_fence_only", "B_no_flush"]:
    sel = [float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]==b and r["keydist"]=="zipf-1.5"]
    if sel:
        v = statistics.median(sel)
        gain = (v-base)/base * 100
        print(f"  {b:<22} = {v:6.2f} Mops  (vs baseline {base:.2f} = {gain:+.1f}%)")
print()
if True:
    b1 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]=="B1_no_flush_only" and r["keydist"]=="zipf-1.5"] or [0])
    b2 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]=="B2_no_fence_only" and r["keydist"]=="zipf-1.5"] or [0])
    b3 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp1" and r["build"]=="B_no_flush" and r["keydist"]=="zipf-1.5"] or [0])
    if b1 > 0 and b2 > 0 and b3 > 0:
        if abs(b1 - b3) / b3 < 0.1:
            print(f"  → flush_line is the dominant cause (B1 ≈ B3, both significantly > baseline)")
        elif abs(b2 - b3) / b3 < 0.1:
            print(f"  → mfence is the dominant cause (B2 ≈ B3)")
        elif b1 > base * 1.5 and b2 > base * 1.5:
            print(f"  → BOTH contribute (B1 and B2 both intermediate gains)")
        else:
            print(f"  → unexpected: B1={b1:.1f}, B2={b2:.1f}, B3={b3:.1f}")

print()
print("=== Exp 2: T scaling at zipf-1.5 (thpt Mops median) ===")
print(f"{'T':>4}{'baseline':>12}{'B_no_flush':>14}{'gain%':>10}")
for T in [8, 16, 32, 64]:
    base_t = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp2" and r["build"]=="baseline" and int(r["T"])==T] or [0])
    bnf_t = statistics.median([float(r["thpt_Mops"]) for r in rows if r["exp"]=="exp2" and r["build"]=="B_no_flush" and int(r["T"])==T] or [0])
    gain = (bnf_t - base_t) / base_t * 100 if base_t > 0 else 0
    print(f"{T:>4}{base_t:>12.2f}{bnf_t:>14.2f}{gain:>9.1f}%")
print()
print("Interpretation:")
print("  - If gain grows monotonically with T → parallelism (mfence-serialization) hypothesis confirmed")
print("  - If gain constant across T → per-op cost reduction (CXL re-fetch cost) hypothesis")
PY
echo ""
echo "OUT: $OUT"
