#!/usr/bin/env bash
# iter-20A Exp 3: YCSB workload-a + workload-c × T × N={0,4,8} on cleaned build.
# Replays iter-17A 8-group scaling format. Params consistent with Exp 1+2.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_exp3_ycsb_${TS}
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1; H1=g2; DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072  # consistent with Exp 1+2
BUILD=cleaned

WORKLOADS="workloada workloadc"
T_LIST="1 2 4 8 16 32 64"
N_LIST="0 4 8"

echo "workload,N,T,rep,thpt_Mops_cluster,r_p50_us,r_p99_us,w_p50_us,w_p99_us,wallclock_s" > $CSV

m() { local f=$1 k=$2; local v=$(grep -oP "^YCSB.*$k=\K[\d.]+" "$f" 2>/dev/null | head -1); [ -n "$v" ] && echo "$v" || echo "0"; }
ns2us() { awk -v n=$1 'BEGIN{printf "%.2f",n/1000.0}'; }

run() {
  local wl=$1 N=$2 T=$3 rep=$4
  local id="${wl}_N${N}_T${T}_rep${rep}"
  local ck=$RANDOM$$ bd=/root/FUSEE_CXL/build-cxl-w1-v1024-${BUILD}
  for h in $H0 $H1; do ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1; done
  sleep 1
  local t0=$(date +%s)
  ssh $H1 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=$wl FUSEE_RING_SHARDS_FACTOR=$N \
    timeout 240 ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  PH1=$!
  ssh $H0 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=$wl FUSEE_RING_SHARDS_FACTOR=$N \
    timeout 240 ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1
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
  echo "$wl,$N,$T,$rep,$tm,$rp50,$rp99,$wp50,$wp99,$wc" >> $CSV
  echo "[$id] thpt=$tm Mops wc=${wc}s"
}

echo "=== Exp 3 YCSB × T × N sweep @ $(date) ==="
for wl in $WORKLOADS; do
  for N in $N_LIST; do
    for T in $T_LIST; do
      for rep in 1 2 3; do
        run $wl $N $T $rep
      done
    done
  done
done
echo "=== sweep done @ $(date) ==="

python3 <<PY | tee $OUT/summary.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
def md(wl,N,T):
    s=[float(r['thpt_Mops_cluster']) for r in rows if r['workload']==wl and r['N']==str(N) and r['T']==str(T)]
    return statistics.median(s) if s else None
for wl in ['workloada','workloadc']:
    print(f"=== {wl} thpt(T,N) Mops cluster median ===")
    print(f"{'N':<4} " + " ".join(f"T={t:>2}" for t in [1,2,4,8,16,32,64]))
    for N in [0,4,8]:
        row=f"{N:<4} "
        for T in [1,2,4,8,16,32,64]:
            v=md(wl,N,T)
            row += f" {v:>5.2f}" if v else f" {'--':>5}"
        print(row)
    print()
PY
