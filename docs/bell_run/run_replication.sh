#!/bin/bash
# Exp 4: Replication factor scan.
# Fixed: 3 MN + 1 CN (b4), YCSB-A, N=8 clients.
# Vary: num_replication in {2,3}, num_idx_rep in {1,2,3}.
set -u
DOCS=/home/yanwang/FUSEE/docs/bell_run
SUMMARY=$DOCS/exp4_replication.csv
echo "num_replication,num_idx_rep,workload,total_ops,failed_ops,tpt_ops_per_sec,exit_code" > $SUMMARY

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
  local NREP=$1 IREP=$2
  # Server config
  sed "s/\"num_replication\":[^,]*/\"num_replication\": $NREP/" $DOCS/server_3mn.json > /tmp/scfg.json
  # Client config
  sed -e "s/\"num_replication\":[^,]*/\"num_replication\": $NREP/" \
      -e "s/\"num_idx_rep\":[^,]*/\"num_idx_rep\": $IREP/" \
      $DOCS/client_3mn.json > /tmp/ccfg.json
  for h in b1 b2 b3; do
    scp /tmp/scfg.json $h:~/FUSEE/build/ycsb-test/server_config.json >/dev/null 2>&1
  done
  scp /tmp/ccfg.json b4:~/FUSEE/build/ycsb-test/client_config.json >/dev/null 2>&1
}

run_one() {
  local NREP=$1 IREP=$2 WL=$3
  local OUT=$DOCS/exp4_rep${NREP}_idx${IREP}_${WL}.log
  deploy_configs $NREP $IREP
  restart_mns
  echo "[$(date +%T)] num_rep=$NREP num_idx_rep=$IREP $WL ..."
  timeout 180 ssh b4 "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL 8 2>&1" > $OUT
  local EC=$?
  if [ $EC -ne 0 ]; then
    echo "  TIMEOUT/FAIL (exit=$EC)"
    ssh b4 "pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
    echo "$NREP,$IREP,$WL,NA,NA,0,$EC" >> $SUMMARY
    return
  fi
  local TOTAL=$(grep '^total:' $OUT | awk '{print $2}')
  local FAIL=$(grep '^failed:' $OUT | awk '{print $2}')
  local TPT=$(grep '^tpt:' $OUT | awk '{print $2}')
  echo "  total=$TOTAL failed=$FAIL tpt=$TPT"
  echo "$NREP,$IREP,$WL,$TOTAL,$FAIL,$TPT,0" >> $SUMMARY
}

# Valid combos: num_idx_rep <= num_replication
for NREP in 2 3; do
  for IREP in 1 $NREP; do
    # dedupe: e.g. NREP=2 IREP=2 and IREP=1 -> 2 configs
    # NREP=3 IREP=1,2,3 -> 3 configs
    run_one $NREP $IREP workloada
    run_one $NREP $IREP workloadc
  done
done

# Extra: NREP=3, IREP=2
run_one 3 2 workloada
run_one 3 2 workloadc

echo
cat $SUMMARY
