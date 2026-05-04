#!/usr/bin/env bash
# iter-8A spare-time verification grid: per-cell residual collapse rate.
# 5 workloads × 2 KV sizes × T=64 cache=on × 5 reps = 50 runs
# ~25 min wallclock budget.
set -u
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0

run_cell() {
  local wl=$1 kv=$2 T=$3 cache=$4 rep=$5
  local cookie=$(date +%s%N)
  ssh g3 'chmod 666 /dev/dax0.0' >/dev/null 2>&1 &
  ssh g4 'chmod 666 /dev/dax0.0' >/dev/null 2>&1 &
  wait
  ssh g3 "cd /root/FUSEE_CXL/build-cxl && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=workload$wl timeout 90 $BIN $DEV $WL/workload$wl.spec_load $WL/workload$wl.spec_trans 65536 200000 2>&1 | grep YCSB" > /tmp/h0.log &
  ssh g4 "cd /root/FUSEE_CXL/build-cxl && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=workload$wl timeout 90 $BIN $DEV $WL/workload$wl.spec_load $WL/workload$wl.spec_trans 65536 200000 2>&1 > /dev/null" &
  wait
  local thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0.log | head -1 | cut -d= -f2)
  local mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
  echo "$mops"
}

OUT=/tmp/iter8A_resid_grid.csv
echo "wl,kv,rep,mops" > $OUT

for wl in a b c d f; do
  for kv in 256 1024; do
    echo "=== wl=$wl kv=$kv T=64 cache=on ===" >&2
    for r in 1 2 3 4 5; do
      m=$(run_cell $wl $kv 64 1 $r)
      echo "  rep $r: $m" >&2
      echo "$wl,$kv,$r,$m" >> $OUT
    done
  done
done
echo "DONE → $OUT" >&2
