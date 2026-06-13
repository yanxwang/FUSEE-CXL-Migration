#!/usr/bin/env bash
# iter-19A Phase 3 — flush+fence A/B isolation sweep.
# 5 builds × 2 cells (c1=cache_buckets 16384, c100=cache_buckets 2097152)
# × 3 reps. Workload-a (R50 U50), zipf-0.99, V=1024, T=64.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase3_flush_iso_${TS}
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=2000000
V=1024
T=64
SPEC_LOAD=/root/FUSEE_CXL/setup/workloads/workloada.spec_load
SPEC_TRANS=/root/FUSEE_CXL/setup/workloads/workloada.spec_trans
REPS=3

BUILDS="bnf bnf-G2 bnf-G3 bnf-G45 bnf-G6"
CELLS="c1 c100"

echo "build,cell,cache_buckets,rep,thpt_Mops_cluster,r_p50_us,r_p99_us,wallclock_s" > $CSV

cell_buckets() {
  case "$1" in
    c1)   echo 16384   ;;
    c100) echo 2097152 ;;
  esac
}

extract_metric() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}

run_cell() {
  local build=$1 cell=$2 rep=$3
  local CB=$(cell_buckets $cell)
  local id="${build}_${cell}_rep${rep}"
  local cookie=$RANDOM$RANDOM$$
  local out_h0=$OUT/raw/${id}_h0.out
  local out_h1=$OUT/raw/${id}_h1.out
  local builddir=/root/FUSEE_CXL/build-cxl-w1-v1024-${build}

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9 2>/dev/null" >/dev/null 2>&1
  done
  sleep 1

  local t0=$(date +%s)
  ssh $H1 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada \
    ./tests/protocol_a_ycsb $DEV $SPEC_LOAD $SPEC_TRANS $NUM_BUCKETS $TRANS_OPS 2>&1" \
    > $out_h1 2>&1 &
  local PH1=$!

  ssh $H0 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada \
    ./tests/protocol_a_ycsb $DEV $SPEC_LOAD $SPEC_TRANS $NUM_BUCKETS $TRANS_OPS 2>&1" \
    > $out_h0 2>&1
  wait $PH1 2>/dev/null

  local t1=$(date +%s)
  local wallclock=$((t1 - t0))

  local thpt_ns_h0=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local thpt_ns_h1=$(extract_metric $out_h1 'trans_agg_thpt' 0)
  local thpt_mops=$(awk -v a=$thpt_ns_h0 -v b=$thpt_ns_h1 'BEGIN{printf "%.3f",(a+b)/1e6}')

  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract_metric $out_h0 'r_p99_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.2f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.2f", n/1000.0}')

  echo "$build,$cell,$CB,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$wallclock" >> $CSV
  echo "[$id] thpt=$thpt_mops Mops r_p50=${r_p50_us}us wc=${wallclock}s"
}

echo "=== Phase 3 isolation sweep started @ $(date) ==="
echo "Builds: $BUILDS"
echo "Cells: $CELLS x reps=$REPS @ workloada zipf-0.99 V=$V T=$T"
echo ""

for build in $BUILDS; do
  echo "== Build $build =="
  for cell in $CELLS; do
    for rep in $(seq 1 $REPS); do
      run_cell $build $cell $rep
    done
  done
done

echo ""
echo "=== sweep done @ $(date) ==="
echo "CSV=$CSV"

# Summary
python3 <<PY | tee $OUT/summary.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print("=== Median thpt (Mops, cluster) ===")
print(f"{'build':<10} {'c1':>8} {'c100':>8} {'gain c1':>10} {'gain c100':>11}")
def med(b, c):
    s = [float(r['thpt_Mops_cluster']) for r in rows if r['build']==b and r['cell']==c]
    return statistics.median(s) if s else None
b0_c1 = med('bnf','c1')
b0_c100 = med('bnf','c100')
print(f"{'bnf':<10} {b0_c1:>8.2f} {b0_c100:>8.2f} {'baseline':>10} {'baseline':>11}")
for b in ['bnf-G2','bnf-G3','bnf-G45','bnf-G6']:
    c1 = med(b,'c1'); c100 = med(b,'c100')
    if c1 is None or c100 is None: continue
    g1 = (c1-b0_c1)/b0_c1*100
    g2 = (c100-b0_c100)/b0_c100*100
    print(f"{b:<10} {c1:>8.2f} {c100:>8.2f} {g1:>9.1f}% {g2:>10.1f}%")
PY
