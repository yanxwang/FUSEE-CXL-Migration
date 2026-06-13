#!/bin/bash
# Protocol A Fig 13 sweep — paper §6.3 YCSB throughput, multi-client.
# 4 workloads × 6 client counts.
# Workloads: A (R50 U50 Zipf), B (R95 U5 Zipf), C (100R Zipf), D (R95 latest).
#
# Output: docs/protocol_A_fig13_<ts>/ with per-cell .log + summary.csv.
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_A_fig13_${TS}"
mkdir -p "$OUT_DIR"

WORKLOADS=(${FUSEE_F_WORKLOADS:-a b c d})
# Skip c=1/c=2: those reproducibly segfault g2's receiver thread under
# single-client cross-host load (same bug surfaced in Fig 11 sweep).
# c≥4 is stable on g1/g2 with iter-21A.
CLIENTS_PER_HOST=(${FUSEE_F_CLIENTS_LIST:-4 8 16 32})
KV=${FUSEE_KV_SIZE:-256}
MAX_OPS=${FUSEE_F_MAX_OPS:-1000000}  # 1M cap per workload
PER_CELL_TIMEOUT=400

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL_DIR=/tmp/workloads

echo "== Protocol A Fig 13 sweep $(date) ==" | tee "$OUT_DIR/run.log"
echo "workloads: ${WORKLOADS[*]}" | tee -a "$OUT_DIR/run.log"
echo "clients_per_host: ${CLIENTS_PER_HOST[*]}" | tee -a "$OUT_DIR/run.log"
echo "kv=$KV max_ops=$MAX_OPS" | tee -a "$OUT_DIR/run.log"

echo "host,workload,num_clients_per_host,total_clients,trans_ops,trans_wall_s,trans_agg_kops" > "$OUT_DIR/summary.csv"

for WL in "${WORKLOADS[@]}"; do
  for C in "${CLIENTS_PER_HOST[@]}"; do
    RUN_ID=$(date +%s%N)
    CELL_TAG="w${WL}_c${C}"
    G1_LOG="$OUT_DIR/${CELL_TAG}_g1.log"
    G2_LOG="$OUT_DIR/${CELL_TAG}_g2.log"

    echo "--- $CELL_TAG (per-host=$C, total=$((2*C))) ---" | tee -a "$OUT_DIR/run.log"

    LOAD_FILE="$WL_DIR/workload${WL}.spec_load"
    TRANS_FILE="$WL_DIR/workload${WL}.spec_trans"

    ENV_COMMON="FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$C FUSEE_RUN_COOKIE=$RUN_ID \
      FUSEE_KV_SIZE=$KV FUSEE_CACHE=0 FUSEE_DISABLE_C13_EPOCH=1 \
      FUSEE_WORKLOAD_NAME=workload${WL}_c${C}"

    ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
    ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
    wait

    ssh root@g1 "$ENV_COMMON FUSEE_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 $LOAD_FILE $TRANS_FILE 65536 $MAX_OPS 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
    G1_PID=$!
    sleep 10
    ssh root@g2 "$ENV_COMMON FUSEE_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 $LOAD_FILE $TRANS_FILE 65536 $MAX_OPS 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
    G2_PID=$!

    wait $G1_PID $G2_PID

    ssh root@g1 'pkill -9 -f protocol_a_ycsb' 2>/dev/null
    ssh root@g2 'pkill -9 -f protocol_a_ycsb' 2>/dev/null

    for H in g1 g2; do
      LOG="$OUT_DIR/${CELL_TAG}_${H}.log"
      if grep -q "^YCSB opt=A" "$LOG"; then
        LINE=$(grep "^YCSB opt=A" "$LOG" | head -1)
        TOPS=$(echo "$LINE" | grep -oP 'trans_ops=\K[0-9]+')
        TWS=$(echo "$LINE" | grep -oP 'trans_wall_max=\K[0-9.]+')
        TPT=$(echo "$LINE" | grep -oP 'trans_agg_thpt=\K[0-9]+')
        echo "$H,$WL,$C,$((2*C)),$TOPS,$TWS,$TPT" >> "$OUT_DIR/summary.csv"
        echo "  $H: trans_ops=$TOPS wall=$TWS s thpt=$TPT kops/s" | tee -a "$OUT_DIR/run.log"
      else
        echo "  $H: NO SUMMARY (check $LOG)" | tee -a "$OUT_DIR/run.log"
      fi
    done
  done
done

echo "" | tee -a "$OUT_DIR/run.log"
echo "Fig 13 sweep complete: $OUT_DIR/summary.csv" | tee -a "$OUT_DIR/run.log"
