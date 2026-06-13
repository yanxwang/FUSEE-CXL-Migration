#!/usr/bin/env bash
# Protocol F YCSB scaling sweep on g1+g2.
#
# Outputs: docs/protocol_F_sweep_<timestamp>/results.csv
#
# Cells: T in {1,2,4,8,16,32} x N in {1,2} x workload in {A_big, C_big}
# Workload sources: ~/FUSEE_CXL/workloads_synth/wl_{A,C}_big.{load,trans}
# (200K trans ops over 1M-key zipf-0.99 keyspace, 100K load INSERTs)
set -u

STAMP=$(date +%Y%m%d_%H%M%S)
OUT=docs/protocol_F_sweep_${STAMP}
mkdir -p "$OUT"
CSV="$OUT/results.csv"
LOGDIR="$OUT/logs"
mkdir -p "$LOGDIR"

NUM_BUCKETS=${NUM_BUCKETS:-32768}
MAX_OPS=${MAX_OPS:-200000}
TS=${TS:-"1 2 4 8 16 32"}
WORKLOADS=${WORKLOADS:-"A_big C_big"}

BIN="./tests/cxl_ycsb_runner_F"

echo "T,N,workload,load_thpt,trans_thpt,w_p50_ns,w_p99_ns,r_p50_ns,r_p99_ns,load_ops,trans_ops" > "$CSV"

for wl in $WORKLOADS; do
  LOAD=/root/FUSEE_CXL/workloads_synth/wl_${wl}.load
  TRANS=/root/FUSEE_CXL/workloads_synth/wl_${wl}.trans
  for T in $TS; do
    for N in 1 2; do
      COOKIE=$(date +%s%N)
      label="${wl}_T${T}_N${N}"
      echo "=== $label ===" | tee -a "$LOGDIR/${label}.log"

      common="FUSEE_RUN_COOKIE=$COOKIE FUSEE_NUM_HOSTS=$N FUSEE_NUM_THREADS=$T"
      cmd0="${common} FUSEE_HOST_ID=0 timeout 120 $BIN /dev/dax0.0 $LOAD $TRANS $NUM_BUCKETS $MAX_OPS"
      cmd1="${common} FUSEE_HOST_ID=1 timeout 120 $BIN /dev/dax0.0 $LOAD $TRANS $NUM_BUCKETS $MAX_OPS"

      ssh root@g1 "cd FUSEE_CXL/build-cxl && $cmd0" > "$LOGDIR/${label}_g1.out" 2>&1 &
      P1=$!
      if [ "$N" = "2" ]; then
        sleep 2
        ssh root@g2 "cd FUSEE_CXL/build-cxl && $cmd1" > "$LOGDIR/${label}_g2.out" 2>&1 &
        P2=$!
      fi
      wait $P1
      [ "$N" = "2" ] && wait $P2

      # Parse g1's output (host 0 is the cluster reporter for these runs).
      line=$(grep '^YCSB ' "$LOGDIR/${label}_g1.out" | tail -1)
      if [ -z "$line" ]; then
        echo "$T,$N,$wl,FAIL,FAIL,FAIL,FAIL,FAIL,FAIL,FAIL,FAIL" >> "$CSV"
        echo "FAIL — no YCSB line" | tee -a "$LOGDIR/${label}.log"
        continue
      fi
      lt=$(echo "$line" | grep -oE 'load_thpt=[0-9]+'    | cut -d= -f2)
      tt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      wp50=$(echo "$line" | grep -oE 'w_p50_ns=[0-9]+'   | cut -d= -f2)
      wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+'   | cut -d= -f2)
      rp50=$(echo "$line" | grep -oE 'r_p50_ns=[0-9]+'   | cut -d= -f2)
      rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+'   | cut -d= -f2)
      lops=$(echo "$line" | grep -oE 'load_ops=[0-9]+'   | cut -d= -f2)
      tops=$(echo "$line" | grep -oE 'trans_ops=[0-9]+'  | cut -d= -f2)
      echo "$T,$N,$wl,$lt,$tt,$wp50,$wp99,$rp50,$rp99,$lops,$tops" >> "$CSV"
      echo "$line" | tee -a "$LOGDIR/${label}.log"
    done
  done
done

echo
echo "=== sweep done ==="
echo "results: $CSV"
column -t -s, "$CSV"
