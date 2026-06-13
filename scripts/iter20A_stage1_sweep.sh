#!/usr/bin/env bash
# iter-20A STAGE 1 verification: HAZARD+RESERVED (hzres) vs STAGING default (bnf).
# 2 builds × 2 workloads (a,c) × 2 cells (c1, c100) × 3 reps = 24 cells
# Then cross-host hash-diff for hzres.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_stage1_${TS}
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1; H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=2000000
V=1024
T=64
REPS=3
BUILDS="bnf hzres"
WORKLOADS="workloada workloadc"
CELLS="c1 c100"

echo "build,workload,cell,cache_buckets,rep,thpt_Mops_cluster,r_p50_us,r_p99_us,wallclock_s" > $CSV

cell_buckets() { case "$1" in c1) echo 16384;; c100) echo 2097152;; esac; }
extract_metric() {
  local f=$1 k=$2 d=$3
  local v=$(grep -oP "^YCSB.*$k=\K[\d.]+" "$f" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$d"
}

run_cell() {
  local build=$1 wl=$2 cell=$3 rep=$4
  local CB=$(cell_buckets $cell)
  local id="${build}_${wl}_${cell}_rep${rep}"
  local cookie=$RANDOM$$
  local out_h0=$OUT/raw/${id}_h0.out
  local out_h1=$OUT/raw/${id}_h1.out
  local builddir=/root/FUSEE_CXL/build-cxl-w1-v1024-${build}

  for h in $H0 $H1; do ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1; done
  sleep 1

  local t0=$(date +%s)
  ssh $H1 "cd $builddir && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V FUSEE_WORKLOAD_NAME=$wl \
    timeout 180 ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/workloads/${wl}.spec_load \
      /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  PH1=$!
  ssh $H0 "cd $builddir && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V FUSEE_WORKLOAD_NAME=$wl \
    timeout 180 ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/workloads/${wl}.spec_load \
      /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1
  wait $PH1 2>/dev/null
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt0=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local thpt1=$(extract_metric $out_h1 'trans_agg_thpt' 0)
  local thpt_mops=$(awk -v a=$thpt0 -v b=$thpt1 'BEGIN{printf "%.3f",(a+b)/1e6}')
  local rp50=$(awk -v n=$(extract_metric $out_h0 'r_p50_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  local rp99=$(awk -v n=$(extract_metric $out_h0 'r_p99_ns' 0) 'BEGIN{printf "%.2f",n/1000.0}')
  echo "$build,$wl,$cell,$CB,$rep,$thpt_mops,$rp50,$rp99,$wc" >> $CSV
  echo "[$id] thpt=$thpt_mops Mops r_p50=${rp50}us wc=${wc}s"
}

echo "=== STAGE 1 sweep @ $(date) ==="
for build in $BUILDS; do
  echo "== $build =="
  for wl in $WORKLOADS; do
    for cell in $CELLS; do
      for rep in $(seq 1 $REPS); do
        run_cell $build $wl $cell $rep
      done
    done
  done
done
echo "=== sweep done @ $(date) ==="

python3 <<PY | tee $OUT/summary.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print("=== STAGE 1: thpt median (Mops, cluster) ===")
print(f"{'build':<8} {'workload':<10} {'c1':>8} {'c100':>8}")
def med(b,wl,c):
    s = [float(r['thpt_Mops_cluster']) for r in rows if r['build']==b and r['workload']==wl and r['cell']==c]
    return statistics.median(s) if s else None
for build in ['bnf','hzres']:
    for wl in ['workloada','workloadc']:
        c1=med(build,wl,'c1'); c100=med(build,wl,'c100')
        c1_s = f"{c1:.2f}" if c1 else "--"
        c100_s = f"{c100:.2f}" if c100 else "--"
        print(f"{build:<8} {wl:<10} {c1_s:>8} {c100_s:>8}")
print()
print("=== hzres vs bnf gain ===")
for wl in ['workloada','workloadc']:
    for c in ['c1','c100']:
        b=med('bnf',wl,c); h=med('hzres',wl,c)
        if b and h:
            gain = (h-b)/b*100
            print(f"  {wl} {c:<6}  bnf={b:6.2f}  hzres={h:6.2f}  gain={gain:+6.1f}%")
PY
