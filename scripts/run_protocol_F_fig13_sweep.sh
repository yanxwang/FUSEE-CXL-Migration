#!/bin/bash
# Protocol F Fig 13 sweep — paper §6.3 YCSB throughput, multi-client.
#
# 4 workloads × 8 client counts = 32 cells.
# Workloads: A (R50 U50 Zipf), B (R95 U5 Zipf), C (100R Zipf), D (R95 latest).
# Per FUSEE config: trans_run = 5 s.
#
# Output: docs/protocol_F_fig13_<ts>/ with per-cell .log + summary.csv.
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_F_fig13_${TS}"
mkdir -p "$OUT_DIR"

WORKLOADS=(a b c d)
CLIENTS_PER_HOST=(1 2 3 4 8 16 32 64)
RUN_MS=5000
PER_CELL_TIMEOUT=400   # LFM attach + load + 5s trans + buffer

BIN=/root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_ycsb
WL_DIR=/tmp/workloads

echo "== Fig 13 sweep $(date) ==" | tee "$OUT_DIR/run.log"
echo "workloads: ${WORKLOADS[*]}" | tee -a "$OUT_DIR/run.log"
echo "clients_per_host: ${CLIENTS_PER_HOST[*]}" | tee -a "$OUT_DIR/run.log"
echo "run_ms=$RUN_MS" | tee -a "$OUT_DIR/run.log"

echo "host,workload,num_clients,total_clients,wall_ms,trans_ops,trans_tpt" > "$OUT_DIR/summary.csv"

for WL in "${WORKLOADS[@]}"; do
  for C in "${CLIENTS_PER_HOST[@]}"; do
    RUN_ID=$(date +%s%N)
    CELL_TAG="w${WL}_c${C}"
    G1_LOG="$OUT_DIR/${CELL_TAG}_g1.log"
    G2_LOG="$OUT_DIR/${CELL_TAG}_g2.log"

    echo "--- $CELL_TAG (per-host=$C, total=$((2*C))) ---" | tee -a "$OUT_DIR/run.log"

    LOAD_FILE="$WL_DIR/workload${WL}.spec_load"
    TRANS_FILE="$WL_DIR/workload${WL}.spec_trans"

    ENV="FUSEE_F_NUM_HOSTS=2 FUSEE_F_NUM_CLIENTS=$C FUSEE_F_RUN_COOKIE=$RUN_ID \
         FUSEE_F_WORKLOAD=$WL FUSEE_F_RUN_MS=$RUN_MS"

    # Re-init DAX between cells.
    ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
    ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
    wait

    ssh root@g1 "$ENV FUSEE_F_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 $LOAD_FILE $TRANS_FILE 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
    G1_PID=$!
    # Poll for "host 0 attach OK" before launching g2 (race-free; LFM
    # init memset is large).  Bounded wait 90s.
    for _ in $(seq 1 90); do
      if grep -q "host 0 attach OK" "$G1_LOG" 2>/dev/null; then break; fi
      sleep 1
    done
    if ! grep -q "host 0 attach OK" "$G1_LOG" 2>/dev/null; then
      echo "  WARN: g1 not 'attach OK' in 90s; launching g2 anyway" | tee -a "$OUT_DIR/run.log"
    fi
    ssh root@g2 "$ENV FUSEE_F_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 $LOAD_FILE $TRANS_FILE 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
    G2_PID=$!

    wait $G1_PID $G2_PID

    ssh root@g1 'pkill -9 -f cxl_kv_ops_F_ycsb' 2>/dev/null
    ssh root@g2 'pkill -9 -f cxl_kv_ops_F_ycsb' 2>/dev/null

    for H in g1 g2; do
      LOG="$OUT_DIR/${CELL_TAG}_${H}.log"
      if grep -q "SUMMARY F_ycsb" "$LOG"; then
        LINE=$(grep "SUMMARY F_ycsb" "$LOG" | head -1)
        WMS=$(echo "$LINE" | grep -oP 'wall_ms=\K[0-9]+')
        OPS=$(echo "$LINE" | grep -oP 'trans_ops=\K[0-9]+')
        TPT=$(echo "$LINE" | grep -oP 'trans_tpt=\K[0-9]+')
        echo "$H,$WL,$C,$((2*C)),$WMS,$OPS,$TPT" >> "$OUT_DIR/summary.csv"
        echo "  $H: wall=$WMS ms ops=$OPS tpt=$TPT" | tee -a "$OUT_DIR/run.log"
      else
        echo "  $H: NO SUMMARY (check $LOG)" | tee -a "$OUT_DIR/run.log"
      fi
    done
  done
done

echo "== sweep done $(date) ==" | tee -a "$OUT_DIR/run.log"
echo "results: $OUT_DIR/"
