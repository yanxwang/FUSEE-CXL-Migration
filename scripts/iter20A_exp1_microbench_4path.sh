#!/usr/bin/env bash
# iter-20A Exp 1: 4-path microbench baseline on cleaned build.
# Replays iter-15A Phase 2 (T sweep, dist=zipf-0.99) + Phase 4 (dist sweep, T=64).
# All other params follow docs/microbench_4path_spec.md.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_exp1_microbench_${TS}
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1; H1=g2; DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000   # per host (= 10 M cluster); per spec
V=1024
CACHE_BUCKETS=131072  # ≈ 10% per spec
TRACE=/tmp/microbench_traces
BUILD=cleaned

SCENS="local_read xhost_read local_write xhost_write"

echo "phase,scenario,keydist,T,rep,thpt_Mops_cluster,r_p50_us,r_p99_us,w_p50_us,w_p99_us,wallclock_s" > $CSV

m() { local f=$1 k=$2; local v=$(grep -oP "^YCSB.*$k=\K[\d.]+" "$f" 2>/dev/null | head -1); [ -n "$v" ] && echo "$v" || echo "0"; }
ns2us() { awk -v n=$1 'BEGIN{printf "%.2f",n/1000.0}'; }

run() {
  local phase=$1 sc=$2 kd=$3 T=$4 rep=$5
  local id="${phase}_${sc}_${kd}_T${T}_rep${rep}"
  local ck=$RANDOM$$
  local bd=/root/FUSEE_CXL/build-cxl-w1-v1024-${BUILD}
  for h in $H0 $H1; do ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1; done
  sleep 1
  local t0=$(date +%s)
  ssh $H1 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=${sc} \
    timeout 240 ./tests/protocol_a_ycsb $DEV ${TRACE}/bench_${sc}_${kd}_h1.spec_load ${TRACE}/bench_${sc}_${kd}_h1.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  local PH1=$!
  ssh $H0 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=${sc} \
    timeout 240 ./tests/protocol_a_ycsb $DEV ${TRACE}/bench_${sc}_${kd}_h0.spec_load ${TRACE}/bench_${sc}_${kd}_h0.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1
  wait $PH1 2>/dev/null
  local t1=$(date +%s)
  local wc=$((t1-t0))
  local th0=$(m $OUT/raw/${id}_h0.out trans_agg_thpt)
  local th1=$(m $OUT/raw/${id}_h1.out trans_agg_thpt)
  local tm=$(awk -v a=$th0 -v b=$th1 'BEGIN{printf "%.3f",(a+b)/1e6}')
  local rp50=$(ns2us $(m $OUT/raw/${id}_h0.out r_p50_ns))
  local rp99=$(ns2us $(m $OUT/raw/${id}_h0.out r_p99_ns))
  local wp50=$(ns2us $(m $OUT/raw/${id}_h0.out w_p50_ns))
  local wp99=$(ns2us $(m $OUT/raw/${id}_h0.out w_p99_ns))
  echo "$phase,$sc,$kd,$T,$rep,$tm,$rp50,$rp99,$wp50,$wp99,$wc" >> $CSV
  echo "[$id] thpt=$tm Mops wc=${wc}s"
}

echo "=== Exp 1 Phase 2 (T sweep, dist=zipf-0.99) @ $(date) ==="
for sc in $SCENS; do
  for T in 1 2 4 8 16 32 64; do
    for rep in 1 2 3; do
      run phase2 $sc zipf-0.99 $T $rep
    done
  done
done

echo "=== Exp 1 Phase 4 (dist sweep, T=64) @ $(date) ==="
for sc in $SCENS; do
  for kd in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
    for rep in 1 2 3; do
      run phase4 $sc $kd 64 $rep
    done
  done
done

echo "=== sweep done @ $(date) ==="

python3 <<PY | tee $OUT/summary.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print("=== Exp 1 Phase 2 — thpt(T) per path, zipf-0.99 (Mops cluster median) ===")
print(f"{'scenario':<14} " + " ".join(f"T={t:>2}" for t in [1,2,4,8,16,32,64]))
def md(p,sc,kd,T):
    s=[float(r['thpt_Mops_cluster']) for r in rows if r['phase']==p and r['scenario']==sc and r['keydist']==kd and r['T']==str(T)]
    return statistics.median(s) if s else None
for sc in ['local_read','xhost_read','local_write','xhost_write']:
    row=f"{sc:<14} "
    for T in [1,2,4,8,16,32,64]:
        v=md('phase2',sc,'zipf-0.99',T)
        row += f" {v:>5.2f}" if v else f" {'--':>5}"
    print(row)
print()
print("=== Exp 1 Phase 4 — thpt(dist) per path, T=64 (Mops cluster median) ===")
print(f"{'scenario':<14} " + " ".join(f"{k:>10}" for k in ['uniform','zipf-0.5','zipf-0.99','zipf-1.5']))
for sc in ['local_read','xhost_read','local_write','xhost_write']:
    row=f"{sc:<14} "
    for kd in ['uniform','zipf-0.5','zipf-0.99','zipf-1.5']:
        v=md('phase4',sc,kd,64)
        row += f" {v:>10.2f}" if v else f" {'--':>10}"
    print(row)
PY
