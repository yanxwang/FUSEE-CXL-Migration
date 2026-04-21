#!/usr/bin/env bash
# Run ycsb_abc_bench on CXL devdax (multi-proc simulating multiple nodes).

set -eu
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/.."

NUM_NODES=${NUM_NODES:-4}
THREADS=${THREADS:-2}
OPS=${OPS:-3000}
DEV_PATH=${DEV_PATH:-/dev/dax0.0}
LOG=${LOG:-$SCRIPT_DIR/ycsb_abc_results_cxl.log}

> "$LOG"
for OPT in A B C; do
    for WL in A C; do
        echo "=== opt=$OPT workload=$WL ==="
        PIDS=()
        for (( NID=0; NID<NUM_NODES; NID++ )); do
            if [[ $NID -eq 0 ]]; then SLEEP=0; else SLEEP=0.3; fi
            (
                sleep "$SLEEP"
                ./bench/ycsb_abc_bench \
                    --opt="$OPT" --workload="$WL" \
                    --nodes=$NUM_NODES --node-id=$NID \
                    --threads=$THREADS --ops=$OPS \
                    --path="$DEV_PATH"
            ) >> "$LOG" 2>&1 &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" || true; done
        echo "  done"
        sleep 2
    done
done
echo ""
echo "All runs complete. Log: $LOG"
