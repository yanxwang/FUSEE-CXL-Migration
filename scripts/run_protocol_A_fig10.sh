#!/bin/bash
# Protocol A Fig 10 — paper §6.2 single-client × N×4-op-type serial latency CDF.
# Runs on g1+g2, single worker per host. Only host 0 client 0 executes ops;
# host 1's worker sits at end barrier.
#
# Output: docs/protocol_A_fig10_<ts>/results/{insert,search,update,delete}_lat-Ap.txt
#         (raw µs latencies per op, one per line) + per-op
#         {insert,search,update,delete}_{local,xhost}_lat-Ap.txt splits.
set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="docs/protocol_A_fig10_${TS}"
mkdir -p "$OUT_DIR"

KEYS=${FUSEE_F_KEYS_PER_CLIENT:-100000}
KV=${FUSEE_KV_SIZE:-256}
PER_CELL_TIMEOUT=600

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb

echo "== Protocol A Fig 10 sweep $(date) ==" | tee "$OUT_DIR/run.log"
echo "keys_per_client=$KEYS kv_size=$KV" | tee -a "$OUT_DIR/run.log"

RUN_ID=$(date +%s%N)
ENV_COMMON="FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=1 FUSEE_RUN_COOKIE=$RUN_ID \
  FUSEE_KV_SIZE=$KV FUSEE_BENCH_MODE=fig10 FUSEE_F_KEYS_PER_CLIENT=$KEYS \
  FUSEE_CACHE=0 FUSEE_DISABLE_C13_EPOCH=1 FUSEE_WORKLOAD_NAME=fig10"

# Re-init DAX.
ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
wait

# Launch g1 first (host 0 = init), g2 second after attach sleep.
ssh root@g1 "$ENV_COMMON FUSEE_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 /dev/null /dev/null 65536 0 2>&1; echo exit_g1=\$?" > "$OUT_DIR/g1.log" 2>&1 &
G1_PID=$!
sleep 10
ssh root@g2 "$ENV_COMMON FUSEE_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 /dev/null /dev/null 65536 0 2>&1; echo exit_g2=\$?" > "$OUT_DIR/g2.log" 2>&1 &
G2_PID=$!

wait $G1_PID $G2_PID

# Force-kill stragglers.
ssh root@g1 'pkill -9 -f protocol_a_ycsb' 2>/dev/null
ssh root@g2 'pkill -9 -f protocol_a_ycsb' 2>/dev/null

# Pull results from g1 (only host 0 wrote the latency files).
mkdir -p "$OUT_DIR/results"
rsync -az root@g1:/root/results/ "$OUT_DIR/results/" 2>&1 | tee -a "$OUT_DIR/run.log"

echo "" | tee -a "$OUT_DIR/run.log"
echo "== Fig 10 outputs ==" | tee -a "$OUT_DIR/run.log"
ls -la "$OUT_DIR/results/" | tee -a "$OUT_DIR/run.log"

for OP in insert search update delete; do
  for SUFFIX in "" "_local" "_xhost"; do
    F="$OUT_DIR/results/${OP}${SUFFIX}_lat-Ap.txt"
    if [ -f "$F" ]; then
      LINES=$(wc -l < "$F")
      P50=$(sort -n "$F" | awk -v p=0.5 'BEGIN{c=0} {a[c++]=$0} END{print a[int(c*p)]}')
      P99=$(sort -n "$F" | awk -v p=0.99 'BEGIN{c=0} {a[c++]=$0} END{print a[int(c*p)]}')
      printf "  %-7s%-7s n=%6s p50=%s us p99=%s us\n" "$OP" "$SUFFIX" "$LINES" "$P50" "$P99" | tee -a "$OUT_DIR/run.log"
    fi
  done
done

echo "" | tee -a "$OUT_DIR/run.log"
echo "fig10 sweep complete: $OUT_DIR" | tee -a "$OUT_DIR/run.log"
