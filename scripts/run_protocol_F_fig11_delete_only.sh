#!/bin/bash
# DELETE-only sweep for Fig 11 — bump kKeysPerClient + kTotalRecords so
# the DELETE phase runs for the full 500 ms timer instead of being
# bounded by the small pre-load slab (100 keys/thread × 12 µs/op = ~1.2 ms,
# which leaves the thread idle for 498 ms).
#
# Each thread pre-loads kKeysPerClient=40000 keys, then runs DELETE for
# 500 ms.  INSERT/SEARCH/UPDATE phases are skipped via *_MS=0.
#
# kTotalRecords = 6M (6 GB pool) covers the worst case at c=128:
#   pre-load = 128 × 40K = 5.12M records < 6M ✓
#
# Output: docs/protocol_F_fig11_delete_only_<ts>/
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_F_fig11_delete_only_${TS}"
mkdir -p "$OUT_DIR"

CLIENTS_PER_HOST=(1 2 3 4 8 16 32 64)
INS_MS=0
READ_MS=0
UPD_MS=0
DEL_MS=500
KEYS_PER_CLIENT=40000
TOTAL_RECORDS=6000000
PER_CELL_TIMEOUT=400

BIN=/root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_micro_throughput

echo "== Fig 11 DELETE-only $(date) ==" | tee "$OUT_DIR/run.log"
echo "clients_per_host: ${CLIENTS_PER_HOST[*]}" | tee -a "$OUT_DIR/run.log"
echo "keys_per_client=$KEYS_PER_CLIENT total_records=$TOTAL_RECORDS del_ms=$DEL_MS" | tee -a "$OUT_DIR/run.log"

echo "host,num_clients,total_clients,delete_tpt" > "$OUT_DIR/summary.csv"

for C in "${CLIENTS_PER_HOST[@]}"; do
  RUN_ID=$(date +%s%N)
  CELL="c${C}"
  G1_LOG="$OUT_DIR/${CELL}_g1.log"
  G2_LOG="$OUT_DIR/${CELL}_g2.log"
  echo "--- $CELL (per-host=$C, total=$((2*C))) ---" | tee -a "$OUT_DIR/run.log"

  ENV="FUSEE_F_NUM_HOSTS=2 FUSEE_F_NUM_CLIENTS=$C FUSEE_F_RUN_COOKIE=$RUN_ID \
       FUSEE_F_INS_MS=$INS_MS FUSEE_F_READ_MS=$READ_MS \
       FUSEE_F_UPD_MS=$UPD_MS FUSEE_F_DEL_MS=$DEL_MS \
       FUSEE_F_KEYS_PER_CLIENT=$KEYS_PER_CLIENT \
       FUSEE_F_TOTAL_RECORDS=$TOTAL_RECORDS"

  ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  wait

  ssh root@g1 "$ENV FUSEE_F_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
  for _ in $(seq 1 120); do
    if grep -q "host 0 attach OK" "$G1_LOG" 2>/dev/null; then break; fi
    sleep 1
  done
  ssh root@g2 "$ENV FUSEE_F_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
  wait

  ssh root@g1 'pkill -9 -f cxl_kv_ops_F_micro_throughput' 2>/dev/null
  ssh root@g2 'pkill -9 -f cxl_kv_ops_F_micro_throughput' 2>/dev/null

  for H in g1 g2; do
    LOG="$OUT_DIR/${CELL}_${H}.log"
    if grep -q "SUMMARY F_micro_tpt" "$LOG"; then
      LINE=$(grep "SUMMARY F_micro_tpt" "$LOG" | head -1)
      DEL=$(echo "$LINE" | grep -oP 'delete_tpt=\K[0-9]+')
      echo "$H,$C,$((2*C)),$DEL" >> "$OUT_DIR/summary.csv"
      echo "  $H: del=$DEL" | tee -a "$OUT_DIR/run.log"
    else
      echo "  $H: NO SUMMARY (check $LOG)" | tee -a "$OUT_DIR/run.log"
    fi
  done
done

echo "== sweep done $(date) ==" | tee -a "$OUT_DIR/run.log"
echo "results: $OUT_DIR/"
