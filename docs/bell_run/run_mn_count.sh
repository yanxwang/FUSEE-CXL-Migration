#!/bin/bash
# Exp 3: Vary MN count (memory_num = 1, 2, 3).
# Client: b4, 8 clients.
# num_replication = memory_num (always full), num_idx_rep = 1.
set -u
DOCS=/home/yanwang/FUSEE/docs/bell_run
SUMMARY=$DOCS/exp3_mn_count.csv
echo "memory_num,workload,total_ops,failed_ops,tpt_ops_per_sec,exit_code" > $SUMMARY

BASE_IPS=("192.168.128.151" "192.168.128.152" "192.168.128.153")
BASE_HOSTS=(b1 b2 b3)

restart_mns() {
  local M=$1
  for h in b1 b2 b3 b4; do
    ssh $h "pkill -9 -f ycsb_test_server 2>/dev/null || true; pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
  done
  sleep 2
  for i in $(seq 0 $((M-1))); do
    h=${BASE_HOSTS[$i]}
    ssh -f $h "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_server $i > /tmp/mn.log 2>&1" >/dev/null 2>&1
  done
  sleep 4
}

deploy_configs() {
  local M=$1
  # Build memory_ips JSON fragment
  local IPS=""
  for i in $(seq 0 $((M-1))); do
    [ -n "$IPS" ] && IPS="$IPS, "
    IPS="${IPS}\"${BASE_IPS[$i]}\""
  done
  local CID=$M   # client server_id = memory_num = init client

  cat > /tmp/scfg.json <<EOF
{
  "role": "SERVER", "conn_type": "IB", "server_id": 0, "udp_port": 2333,
  "memory_num": $M,
  "memory_ips": [$IPS],
  "ib_dev_id": 1, "ib_port_id": 1, "ib_gid_idx": 0,
  "server_base_addr": "0x10000000", "server_data_len": 4294967296,
  "block_size": 67108864, "subblock_size": 256, "client_local_size": 1073741824,
  "num_replication": $M, "workload_run_time": 10,
  "main_core_id": 0, "poll_core_id": 1, "bg_core_id": 2, "gc_core_id": 3
}
EOF
  cat > /tmp/ccfg.json <<EOF
{
  "role": "CLIENT", "conn_type": "IB", "server_id": $CID, "udp_port": 2333,
  "memory_num": $M,
  "memory_ips": [$IPS],
  "ib_dev_id": 1, "ib_port_id": 1, "ib_gid_idx": 0,
  "server_base_addr": "0x10000000", "server_data_len": 4294967296,
  "block_size": 67108864, "subblock_size": 256, "client_local_size": 1073741824,
  "num_replication": $M, "num_idx_rep": 1, "num_coroutines": 8,
  "miss_rate_threash": 0.1, "workload_run_time": 10,
  "main_core_id": 0, "poll_core_id": 1, "bg_core_id": 2, "gc_core_id": 3
}
EOF
  for i in $(seq 0 $((M-1))); do
    scp /tmp/scfg.json ${BASE_HOSTS[$i]}:~/FUSEE/build/ycsb-test/server_config.json >/dev/null 2>&1
  done
  scp /tmp/ccfg.json b4:~/FUSEE/build/ycsb-test/client_config.json >/dev/null 2>&1
}

run_one() {
  local M=$1 WL=$2
  local OUT=$DOCS/exp3_mn${M}_${WL}.log
  deploy_configs $M
  restart_mns $M
  echo "[$(date +%T)] mn=$M $WL ..."
  timeout 180 ssh b4 "cd ~/FUSEE/build/ycsb-test && stdbuf -oL numactl -N 0 -m 0 ./ycsb_test_multi_client ./client_config.json $WL 8 2>&1" > $OUT
  local EC=$?
  if [ $EC -ne 0 ]; then
    echo "  TIMEOUT/FAIL (exit=$EC)"
    ssh b4 "pkill -9 -f ycsb_test_multi 2>/dev/null || true" >/dev/null 2>&1
    echo "$M,$WL,NA,NA,0,$EC" >> $SUMMARY
    return
  fi
  local TOTAL=$(grep '^total:' $OUT | awk '{print $2}')
  local FAIL=$(grep '^failed:' $OUT | awk '{print $2}')
  local TPT=$(grep '^tpt:' $OUT | awk '{print $2}')
  echo "  total=$TOTAL failed=$FAIL tpt=$TPT"
  echo "$M,$WL,$TOTAL,$FAIL,$TPT,0" >> $SUMMARY
}

# Note: M=1 triggers FUSEE's num_rep=1 bug, but we test it anyway for completeness.
# Expected: M=1 workloada will report high failures / wrong counts.
for M in 1 2 3; do
  run_one $M workloada
  run_one $M workloadc
done
echo
cat $SUMMARY
