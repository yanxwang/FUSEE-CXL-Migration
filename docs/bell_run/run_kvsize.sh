#!/bin/bash
# Exp 5: Vary subblock_size (KV slot size).
# Setup: 3 MN + 1 CN (b4), YCSB-C, N=8.
set -u
DOCS=/home/yanwang/FUSEE/docs/bell_run
SUMMARY=$DOCS/exp5_kvsize.csv
echo "subblock_size,workload,total_ops,failed_ops,tpt_ops_per_sec,exit_code" > $SUMMARY

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

deploy_configs() {
  local SUB=$1
  sed -e "s/\"subblock_size\":[^,]*/\"subblock_size\": $SUB/" $DOCS/server_3mn.json > /tmp/scfg.json
  sed -e "s/\"subblock_size\":[^,]*/\"subblock_size\": $SUB/" $DOCS/client_3mn.json > /tmp/ccfg.json
  # revert any prior num_replication changes: keep at 3 (the server_3mn default)
  for h in b1 b2 b3; do
    scp /tmp/scfg.json $h:~/FUSEE/build/ycsb-test/server_config.json >/dev/null 2>&1
  done
  scp /tmp/ccfg.json b4:~/FUSEE/build/ycsb-test/client_config.json >/dev/null 2>&1
}

run_one() {
  local SUB=$1 WL=$2
  local OUT=$DOCS/exp5_sub${SUB}_${WL}.log
  deploy_configs $SUB
  restart_mns
  echo "[$(date +%T)] subblock=$SUB $WL ..."
  timeout 180 ssh b4 "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL 8 2>&1" > $OUT
  local EC=$?
  if [ $EC -ne 0 ]; then
    echo "  TIMEOUT/FAIL (exit=$EC)"
    ssh b4 "pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
    echo "$SUB,$WL,NA,NA,0,$EC" >> $SUMMARY
    return
  fi
  local TOTAL=$(grep '^total:' $OUT | awk '{print $2}')
  local FAIL=$(grep '^failed:' $OUT | awk '{print $2}')
  local TPT=$(grep '^tpt:' $OUT | awk '{print $2}')
  echo "  total=$TOTAL failed=$FAIL tpt=$TPT"
  echo "$SUB,$WL,$TOTAL,$FAIL,$TPT,0" >> $SUMMARY
}

for SUB in 256 512 1024; do
  run_one $SUB workloada
  run_one $SUB workloadc
done
echo
cat $SUMMARY
