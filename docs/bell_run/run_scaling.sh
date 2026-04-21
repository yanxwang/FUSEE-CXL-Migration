#!/bin/bash
# Bell cluster Exp 2: YCSB scaling on 1 CN (b4) with 3 MN (b1/b2/b3)
set -u
DOCS=/home/yanwang/FUSEE/docs/bell_run
SUMMARY=$DOCS/exp2_scaling.csv

if [ ! -f $SUMMARY ] || [ "${1:-}" = "--reset" ]; then
  echo "workload,num_clients,total_ops,failed_ops,tpt_ops_per_sec,exit_code" > $SUMMARY
fi

restart_mns() {
  for h in b1 b2 b3 b4; do
    ssh $h "pkill -9 -f ycsb_test_server 2>/dev/null || true; pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
  done
  sleep 2
  for i in 1 2 3; do
    h=b$i; sid=$((i-1))
    ssh -f $h "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server $sid > /tmp/mn.log 2>&1" >/dev/null 2>&1
  done
  sleep 4
}

run_one() {
  local WL=$1 N=$2
  local OUT=$DOCS/exp2_${WL}_n${N}.log
  if grep -qE "^$WL,$N,[0-9]+,[0-9]+,[0-9]+,0$" $SUMMARY; then
    echo "[$(date +%T)] $WL N=$N skip"; return
  fi
  restart_mns
  echo "[$(date +%T)] $WL N=$N ..."
  timeout 180 ssh b4 "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL $N 2>&1" > $OUT
  local EC=$?
  if [ $EC -ne 0 ]; then
    echo "  TIMEOUT/FAIL (exit=$EC)"
    ssh b4 "pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
    echo "$WL,$N,NA,NA,0,$EC" >> $SUMMARY
    return
  fi
  local TOTAL=$(grep '^total:' $OUT | awk '{print $2}')
  local FAIL=$(grep '^failed:' $OUT | awk '{print $2}')
  local TPT=$(grep '^tpt:' $OUT | awk '{print $2}')
  echo "  total=$TOTAL failed=$FAIL tpt=$TPT"
  echo "$WL,$N,$TOTAL,$FAIL,$TPT,0" >> $SUMMARY
}

# 1 .. 14 (max per-CN before NUMA crash)
for WL in workloada workloadc; do
  for N in 1 2 4 7 8 14; do
    run_one $WL $N
  done
done
echo "=== DONE ==="
cat $SUMMARY
