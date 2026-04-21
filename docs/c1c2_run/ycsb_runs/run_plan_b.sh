#!/bin/bash
# Plan B: 2 MN + 2 CN, 8 clients per CN, run both concurrently.
set -u
DOCS=/home/yanwang/FUSEE/docs/c1c2_run/ycsb_runs

restart_mns() {
  ssh c1 "pkill -9 -f ycsb_test_server 2>/dev/null || true; pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
  ssh c2 "pkill -9 -f ycsb_test_server 2>/dev/null || true; pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
  sleep 2
  ssh -f c1 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server 0 > /tmp/mn0.log 2>&1" >/dev/null 2>&1
  ssh -f c2 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server 1 > /tmp/mn1.log 2>&1" >/dev/null 2>&1
  sleep 4
}

# Make sure CN_A is server_id=2 (init) and CN_B is server_id=10
ssh c1 "sed -i 's/\"server_id\": [0-9]*,/\"server_id\": 2,/' /root/FUSEE/build/ycsb-test/client_config.json"
ssh c2 "sed -i 's/\"server_id\": [0-9]*,/\"server_id\": 10,/' /root/FUSEE/build/ycsb-test/client_config.json"

for WL in workloada workloadc; do
  restart_mns
  echo "[$(date +%T)] Plan B: $WL, 2 CNs × 8 clients"
  ssh c1 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL 8 > /tmp/cn_a_$WL.log 2>&1" > $DOCS/planB_${WL}_cnA.log 2>&1 &
  PIDA=$!
  sleep 3  # stagger so CN_A wins load race
  ssh c2 "cd /root/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL 8 > /tmp/cn_b_$WL.log 2>&1" > $DOCS/planB_${WL}_cnB.log 2>&1 &
  PIDB=$!
  # Wait with timeout
  TIMED_OUT=0
  wait $PIDA 2>/dev/null || TIMED_OUT=1
  wait $PIDB 2>/dev/null || TIMED_OUT=1
  if [ $TIMED_OUT -ne 0 ]; then
    ssh c1 "pkill -9 -f ycsb_test_multi 2>/dev/null || true"
    ssh c2 "pkill -9 -f ycsb_test_multi 2>/dev/null || true"
  fi
  ssh c1 "cat /tmp/cn_a_$WL.log" > $DOCS/planB_${WL}_cnA.log
  ssh c2 "cat /tmp/cn_b_$WL.log" > $DOCS/planB_${WL}_cnB.log
  TPT_A=$(grep '^tpt:' $DOCS/planB_${WL}_cnA.log | awk '{print $2}')
  FAIL_A=$(grep '^failed:' $DOCS/planB_${WL}_cnA.log | awk '{print $2}')
  TPT_B=$(grep '^tpt:' $DOCS/planB_${WL}_cnB.log | awk '{print $2}')
  FAIL_B=$(grep '^failed:' $DOCS/planB_${WL}_cnB.log | awk '{print $2}')
  echo "  CN_A: tpt=$TPT_A failed=$FAIL_A"
  echo "  CN_B: tpt=$TPT_B failed=$FAIL_B"
  echo "$WL,A,$TPT_A,$FAIL_A" >> $DOCS/plan_b_results.csv
  echo "$WL,B,$TPT_B,$FAIL_B" >> $DOCS/plan_b_results.csv
done
echo
echo "=== Plan B DONE ==="
cat $DOCS/plan_b_results.csv
