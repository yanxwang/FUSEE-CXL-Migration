#!/usr/bin/env bash
# iter-19A Phase 3 single-host sensitivity sweep.
# 5 builds × 2 cells (c1+c100) × 3 reps × 1 host = 30 cells.
# Single-host config forces 100% local ops → no xhost RTT masking → LR/LW
# flush+fence isolation effects become measurable.

set -u
ROOT=/home/yanwang/FUSEE
OUTDIR=${1:-$(ls -dt $ROOT/docs/iter19A_phase3_flush_iso_* | head -1)}
H0=g1
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=2000000
V=1024
T=64
SPEC_LOAD=/root/FUSEE_CXL/setup/workloads/workloada.spec_load
SPEC_TRANS=/root/FUSEE_CXL/setup/workloads/workloada.spec_trans
REPS=3

CSV=$OUTDIR/grid_singlehost.csv
mkdir -p $OUTDIR/raw_singlehost
echo "build,cell,cache_buckets,rep,thpt_Mops_host,r_p50_us,r_p99_us,w_p50_us,w_p99_us,wallclock_s" > $CSV

extract_metric() {
  local f=$1 k=$2 d=$3
  local v=$(grep -oP "^YCSB.*$k=\K[\d.]+" "$f" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$d"
}

cell_buckets() {
  case "$1" in c1) echo 16384 ;; c100) echo 2097152 ;; esac
}

run_one() {
  local build=$1 cell=$2 rep=$3
  local CB=$(cell_buckets $cell)
  local id="${build}_${cell}_rep${rep}"
  local out=$OUTDIR/raw_singlehost/${id}.out
  local builddir=/root/FUSEE_CXL/build-cxl-w1-v1024-${build}
  ssh $H0 "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9 2>/dev/null" >/dev/null 2>&1
  sleep 1
  local t0=$(date +%s)
  ssh $H0 "cd $builddir && \
    FUSEE_NUM_HOSTS=1 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$RANDOM$$ FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada \
    ./tests/protocol_a_ycsb $DEV $SPEC_LOAD $SPEC_TRANS $NUM_BUCKETS $TRANS_OPS 2>&1" > $out 2>&1
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt_ns=$(extract_metric $out 'trans_agg_thpt' 0)
  local thpt_mops=$(awk -v t=$thpt_ns 'BEGIN{printf "%.3f", t/1e6}')
  local rp50=$(awk -v n=$(extract_metric $out 'r_p50_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  local rp99=$(awk -v n=$(extract_metric $out 'r_p99_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  local wp50=$(awk -v n=$(extract_metric $out 'w_p50_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  local wp99=$(awk -v n=$(extract_metric $out 'w_p99_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  echo "$build,$cell,$CB,$rep,$thpt_mops,$rp50,$rp99,$wp50,$wp99,$wc" >> $CSV
  echo "[$id] thpt=$thpt_mops Mops r_p50=${rp50}us wc=${wc}s"
}

echo "## single-host sensitivity sweep @ $(date)"
echo "## workload-a R50 U50, V=$V T=$T, FUSEE_NUM_HOSTS=1, $TRANS_OPS trans ops"
echo ""

for build in bnf bnf-G2 bnf-G3 bnf-G45 bnf-G6; do
  echo "== $build =="
  for cell in c1 c100; do
    for rep in 1 2 3; do
      run_one $build $cell $rep
    done
  done
done

echo ""
echo "## sweep done @ $(date)"

python3 <<PY | tee $OUTDIR/summary_singlehost.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print("=== single-host median thpt (Mops, 1-host) ===")
print(f"{'build':<10} {'c1':>8} {'c100':>8} {'gain c1':>10} {'gain c100':>11}")
def med(b, c):
    s = [float(r['thpt_Mops_host']) for r in rows if r['build']==b and r['cell']==c]
    return statistics.median(s) if s else None
b0_c1 = med('bnf','c1'); b0_c100 = med('bnf','c100')
print(f"{'bnf':<10} {b0_c1:>8.2f} {b0_c100:>8.2f} {'baseline':>10} {'baseline':>11}")
for b in ['bnf-G2','bnf-G3','bnf-G45','bnf-G6']:
    c1 = med(b,'c1'); c100 = med(b,'c100')
    if c1 is None or c100 is None: continue
    g1_pct = (c1-b0_c1)/b0_c1*100; g2_pct = (c100-b0_c100)/b0_c100*100
    print(f"{b:<10} {c1:>8.2f} {c100:>8.2f} {g1_pct:>9.1f}% {g2_pct:>10.1f}%")
PY
