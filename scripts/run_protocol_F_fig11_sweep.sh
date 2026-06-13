#!/bin/bash
# Protocol F Fig 11 sweep — paper §6.2 micro throughput, multi-client.
#
# Sweeps clients-per-host in {1, 2, 3, 4, 8, 16, 32, 64} on g1 + g2.
# Per FUSEE micro_test.cc timer settings: INSERT 500 ms, SEARCH 5000 ms,
# UPDATE 5000 ms, DELETE 500 ms.
#
# Output: docs/protocol_F_fig11_<ts>/ with per-cell .log + summary.csv.
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_F_fig11_${TS}"
mkdir -p "$OUT_DIR"

CLIENTS_PER_HOST=(1 2 3 4 8 16 32 64)
INS_MS=500
READ_MS=5000
UPD_MS=5000
DEL_MS=500
PER_CELL_TIMEOUT=300   # 17 GB LFM attach + 4 phases (12s) + buffer

BIN=/root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_micro_throughput

echo "== Fig 11 sweep $(date) ==" | tee "$OUT_DIR/run.log"
echo "clients_per_host: ${CLIENTS_PER_HOST[*]}" | tee -a "$OUT_DIR/run.log"
echo "ins_ms=$INS_MS read_ms=$READ_MS upd_ms=$UPD_MS del_ms=$DEL_MS" | tee -a "$OUT_DIR/run.log"

echo "host,num_clients,total_clients,insert_tpt,search_tpt,update_tpt,delete_tpt" > "$OUT_DIR/summary.csv"

for C in "${CLIENTS_PER_HOST[@]}"; do
  RUN_ID=$(date +%s%N)
  CELL_TAG="c${C}"
  G1_LOG="$OUT_DIR/${CELL_TAG}_g1.log"
  G2_LOG="$OUT_DIR/${CELL_TAG}_g2.log"

  echo "--- $CELL_TAG (per-host=$C, total=$((2*C))) ---" | tee -a "$OUT_DIR/run.log"

  ENV="FUSEE_F_NUM_HOSTS=2 FUSEE_F_NUM_CLIENTS=$C FUSEE_F_RUN_COOKIE=$RUN_ID \
       FUSEE_F_INS_MS=$INS_MS FUSEE_F_READ_MS=$READ_MS \
       FUSEE_F_UPD_MS=$UPD_MS FUSEE_F_DEL_MS=$DEL_MS"

  # Re-init DAX in case prior cell left state behind.
  ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  wait

  # Launch g1 (host 0, init).  Wait until "attach OK" appears in its log
  # before launching g2 — Fig 11 binary's LFM table at num_buckets=65536
  # is ~17 GB and the memset takes much longer than a fixed sleep.  Race-
  # free attach handshake via log-poll.
  ssh root@g1 "$ENV FUSEE_F_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
  G1_PID=$!
  # Poll for "host 0 attach OK" in the log (bounded wait, max 90s).
  for _ in $(seq 1 90); do
    if grep -q "host 0 attach OK" "$G1_LOG" 2>/dev/null; then break; fi
    sleep 1
  done
  if ! grep -q "host 0 attach OK" "$G1_LOG" 2>/dev/null; then
    echo "  WARN: g1 did not reach 'attach OK' in 90 s; launching g2 anyway" | tee -a "$OUT_DIR/run.log"
  fi
  ssh root@g2 "$ENV FUSEE_F_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
  G2_PID=$!

  wait $G1_PID $G2_PID

  # Force-kill stragglers.
  ssh root@g1 'pkill -9 -f cxl_kv_ops_F_micro_throughput' 2>/dev/null
  ssh root@g2 'pkill -9 -f cxl_kv_ops_F_micro_throughput' 2>/dev/null

  # Parse SUMMARY lines from both hosts and aggregate.
  for H in g1 g2; do
    LOG="$OUT_DIR/${CELL_TAG}_${H}.log"
    if grep -q "SUMMARY F_micro_tpt" "$LOG"; then
      LINE=$(grep "SUMMARY F_micro_tpt" "$LOG" | head -1)
      INS=$(echo "$LINE" | grep -oP 'insert_tpt=\K[0-9]+')
      SEA=$(echo "$LINE" | grep -oP 'search_tpt=\K[0-9]+')
      UPD=$(echo "$LINE" | grep -oP 'update_tpt=\K[0-9]+')
      DEL=$(echo "$LINE" | grep -oP 'delete_tpt=\K[0-9]+')
      echo "$H,$C,$((2*C)),$INS,$SEA,$UPD,$DEL" >> "$OUT_DIR/summary.csv"
      echo "  $H: ins=$INS sea=$SEA upd=$UPD del=$DEL" | tee -a "$OUT_DIR/run.log"
    else
      echo "  $H: NO SUMMARY (check $LOG)" | tee -a "$OUT_DIR/run.log"
    fi
  done
done

echo "== sweep done $(date) ==" | tee -a "$OUT_DIR/run.log"
echo "results: $OUT_DIR/"
