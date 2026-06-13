#!/usr/bin/env bash
# iter-20A STAGE 3 post-deletion re-verify.
# cleaned (HAZARD+RESERVED, no TLS, no STAGING/RCU/BATCHED) vs bnf baseline.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_stage3_${TS}
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1; H1=g2; DEV=/dev/dax0.0
NUM_BUCKETS=8388608; TRANS_OPS=2000000
V=1024; T=64; REPS=3
BUILDS="bnf cleaned"
WORKLOADS="workloada workloadc"
CELLS="c1 c100"

echo "build,workload,cell,cache_buckets,rep,thpt_Mops_cluster,r_p50_us,r_p99_us,wallclock_s" > $CSV
cb() { case "$1" in c1) echo 16384;; c100) echo 2097152;; esac; }
m() { local f=$1 k=$2 d=$3; local v=$(grep -oP "^YCSB.*$k=\K[\d.]+" "$f" 2>/dev/null | head -1); [ -n "$v" ] && echo "$v" || echo "$d"; }

run() {
  local b=$1 wl=$2 c=$3 r=$4
  local CB=$(cb $c) id="${b}_${wl}_${c}_rep${r}"
  local ck=$RANDOM$$ bd=/root/FUSEE_CXL/build-cxl-w1-v1024-${b}
  local o0=$OUT/raw/${id}_h0.out o1=$OUT/raw/${id}_h1.out
  for h in $H0 $H1; do ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1; done
  sleep 1
  local t0=$(date +%s)
  ssh $H1 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$ck FUSEE_REP=$r FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V FUSEE_WORKLOAD_NAME=$wl \
    timeout 180 ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $o1 2>&1 &
  PH1=$!
  ssh $H0 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$ck FUSEE_REP=$r FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=$CB FUSEE_KV_SIZE=$V FUSEE_WORKLOAD_NAME=$wl \
    timeout 180 ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $o0 2>&1
  wait $PH1 2>/dev/null
  local t1=$(date +%s)
  local wc=$((t1-t0))
  local th0=$(m $o0 trans_agg_thpt 0) th1=$(m $o1 trans_agg_thpt 0)
  local tm=$(awk -v a=$th0 -v b=$th1 'BEGIN{printf "%.3f",(a+b)/1e6}')
  local p50=$(awk -v n=$(m $o0 r_p50_ns 0) 'BEGIN{printf "%.2f",n/1000.0}')
  local p99=$(awk -v n=$(m $o0 r_p99_ns 0) 'BEGIN{printf "%.2f",n/1000.0}')
  echo "$b,$wl,$c,$CB,$r,$tm,$p50,$p99,$wc" >> $CSV
  echo "[$id] thpt=$tm Mops wc=${wc}s"
}

echo "=== STAGE 3 sweep @ $(date) ==="
for b in $BUILDS; do
  echo "== $b =="
  for wl in $WORKLOADS; do for c in $CELLS; do for r in $(seq 1 $REPS); do run $b $wl $c $r; done; done; done
done
echo "=== sweep done @ $(date) ==="

python3 <<PY | tee $OUT/summary.txt
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print("=== STAGE 3 median thpt (Mops, cluster) ===")
print(f"{'build':<10} {'workload':<10} {'c1':>8} {'c100':>8}")
def md(b,wl,c):
    s=[float(r['thpt_Mops_cluster']) for r in rows if r['build']==b and r['workload']==wl and r['cell']==c]
    return statistics.median(s) if s else None
for b in ['bnf','cleaned']:
    for wl in ['workloada','workloadc']:
        c1=md(b,wl,'c1'); c100=md(b,wl,'c100')
        c1s=f"{c1:.2f}" if c1 else "--"; c100s=f"{c100:.2f}" if c100 else "--"
        print(f"{b:<10} {wl:<10} {c1s:>8} {c100s:>8}")
print()
print("=== cleaned vs bnf gain ===")
for wl in ['workloada','workloadc']:
    for c in ['c1','c100']:
        b=md('bnf',wl,c); cl=md('cleaned',wl,c)
        if b and cl:
            g=(cl-b)/b*100
            print(f"  {wl} {c:<6}  bnf={b:6.2f}  cleaned={cl:6.2f}  gain={g:+6.1f}%")
PY
