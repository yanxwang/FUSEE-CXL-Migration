#!/bin/bash
# Protocol A Fig 11 sweep — paper §6.2 micro throughput, multi-client.
# Sweeps clients-per-host in {1, 2, 4, 8, 16, 32} on g1 + g2.
#
# Output: docs/protocol_A_fig11_<ts>/ with per-cell .log + summary.csv.
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_A_fig11_${TS}"
mkdir -p "$OUT_DIR"

CLIENTS_PER_HOST=(${FUSEE_F_CLIENTS_LIST:-1 2 4 8 16 32})
# Paper convention: pre-load enough keys per client that SEARCH/UPDATE working
# set defeats the DRAM cache_pool (16K buckets ≈ 16K cached entries). With
# K=10000 × c=64 = 1.28M keys, only ~1 % stays cache-hot.
KEYS_PER_CLIENT=${FUSEE_F_KEYS_PER_CLIENT:-10000}
# Max shards (1 worker per write_ring): removes per-shard fetch_add contention.
# Peer-side single receiver thread is the next bottleneck.
RING_SHARDS_FACTOR=${FUSEE_RING_SHARDS_FACTOR:-1}
KV=${FUSEE_KV_SIZE:-256}
INS_MS=${FUSEE_F_INS_MS:-500}
READ_MS=${FUSEE_F_READ_MS:-5000}
UPD_MS=${FUSEE_F_UPD_MS:-5000}
DEL_MS=${FUSEE_F_DEL_MS:-500}
PER_CELL_TIMEOUT=300

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb

echo "== Protocol A Fig 11 sweep $(date) ==" | tee "$OUT_DIR/run.log"
echo "clients_per_host: ${CLIENTS_PER_HOST[*]}" | tee -a "$OUT_DIR/run.log"
echo "keys_per_client=$KEYS_PER_CLIENT kv=$KV ins_ms=$INS_MS read_ms=$READ_MS upd_ms=$UPD_MS del_ms=$DEL_MS" | tee -a "$OUT_DIR/run.log"

echo "host,num_clients_per_host,total_clients,insert_tpt,search_tpt,update_tpt,delete_tpt" > "$OUT_DIR/summary.csv"

for C in "${CLIENTS_PER_HOST[@]}"; do
  RUN_ID=$(date +%s%N)
  CELL_TAG="c${C}"
  G1_LOG="$OUT_DIR/${CELL_TAG}_g1.log"
  G2_LOG="$OUT_DIR/${CELL_TAG}_g2.log"

  echo "--- $CELL_TAG (per-host=$C, total=$((2*C))) ---" | tee -a "$OUT_DIR/run.log"

  ENV_COMMON="FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$C FUSEE_RUN_COOKIE=$RUN_ID \
    FUSEE_KV_SIZE=$KV FUSEE_BENCH_MODE=fig11 FUSEE_F_KEYS_PER_CLIENT=$KEYS_PER_CLIENT \
    FUSEE_RING_SHARDS_FACTOR=$RING_SHARDS_FACTOR \
    FUSEE_CACHE=0 FUSEE_DISABLE_C13_EPOCH=1 FUSEE_WORKLOAD_NAME=fig11_c$C \
    FUSEE_F_INS_MS=$INS_MS FUSEE_F_READ_MS=$READ_MS FUSEE_F_UPD_MS=$UPD_MS FUSEE_F_DEL_MS=$DEL_MS"

  # Re-init DAX before every cell.
  ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
  wait

  ssh root@g1 "$ENV_COMMON FUSEE_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 /dev/null /dev/null 65536 0 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
  G1_PID=$!
  sleep 10
  ssh root@g2 "$ENV_COMMON FUSEE_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 /dev/null /dev/null 65536 0 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
  G2_PID=$!

  wait $G1_PID $G2_PID

  # Force-kill stragglers.
  ssh root@g1 'pkill -9 -f protocol_a_ycsb' 2>/dev/null
  ssh root@g2 'pkill -9 -f protocol_a_ycsb' 2>/dev/null

  for H in g1 g2; do
    LOG="$OUT_DIR/${CELL_TAG}_${H}.log"
    if grep -q "SUMMARY A_micro_tpt" "$LOG"; then
      LINE=$(grep "SUMMARY A_micro_tpt" "$LOG" | head -1)
      INS=$(echo "$LINE" | grep -oP 'insert_tpt=\K[0-9]+')
      SEA=$(echo "$LINE" | grep -oP 'search_tpt=\K[0-9]+')
      UPD=$(echo "$LINE" | grep -oP 'update_tpt=\K[0-9]+')
      DEL=$(echo "$LINE" | grep -oP 'delete_tpt=\K[0-9]+')
      echo "$H,$C,$((2*C)),$INS,$SEA,$UPD,$DEL" >> "$OUT_DIR/summary.csv"
      echo "  $H: ins=$INS sea=$SEA upd=$UPD del=$DEL" | tee -a "$OUT_DIR/run.log"
    else
      echo "  $H: NO SUMMARY" | tee -a "$OUT_DIR/run.log"
    fi
  done
done

echo "" | tee -a "$OUT_DIR/run.log"
echo "Fig 11 sweep complete: $OUT_DIR/summary.csv" | tee -a "$OUT_DIR/run.log"
