#!/bin/bash
# YCSB scaling test driver.
# Uses ssh -f to properly detach MN servers.

set -u
DOCS=/home/yanwang/FUSEE/docs/c1c2_run/ycsb_runs
SUMMARY=$DOCS/scaling_results.csv

if [ ! -f $SUMMARY ] || [ "${1:-}" = "--reset" ]; then
  echo "workload,num_clients,total_ops,failed_ops,tpt_ops_per_sec,exit_code" > $SUMMARY
fi

restart_mns() {
  ssh c1 "pkill -9 -f ycsb_test_server 2>/dev/null || true" >/dev/null 2>&1
  ssh c2 "pkill -9 -f ycsb_test_server 2>/dev/null || true; pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
  sleep 2
  ssh -f c1 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server 0 > /tmp/mn0.log 2>&1" >/dev/null 2>&1
  ssh -f c2 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server 1 > /tmp/mn1.log 2>&1" >/dev/null 2>&1
  sleep 4
}

run_one() {
  local WL=$1
  local N=$2
  local OUT=$DOCS/${WL}_n${N}.log

  restart_mns
  echo "[$(date +%T)] $WL N=$N ..."
  timeout 180 ssh c2 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL $N 2>&1" > $OUT
  local EC=$?
  if [ $EC -ne 0 ]; then
    echo "  TIMEOUT/FAIL (exit=$EC)"
    ssh c2 "pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
    echo "$WL,$N,NA,NA,0,$EC" >> $SUMMARY
    return
  fi
  local TOTAL=$(grep '^total:' $OUT | awk '{print $2}')
  local FAILED=$(grep '^failed:' $OUT | awk '{print $2}')
  local TPT=$(grep '^tpt:' $OUT | awk '{print $2}')
  echo "  total=$TOTAL failed=$FAILED tpt=$TPT"
  echo "$WL,$N,$TOTAL,$FAILED,$TPT,0" >> $SUMMARY
}

WLS=(workloada workloadc)
NS=(2 4 8 16 32)
for WL in "${WLS[@]}"; do
  for N in "${NS[@]}"; do
    # Skip if already succeeded
    if grep -qE "^$WL,$N,[0-9]+,[0-9]+,[0-9]+,0$" $SUMMARY; then
      echo "[$(date +%T)] $WL N=$N skip (already done)"
      continue
    fi
    run_one $WL $N
  done
done

echo
echo "=== SCALING DONE ==="
cat $SUMMARY
