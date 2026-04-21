#!/usr/bin/env bash
# Scan thread count per node for all 3 options, YCSB A workload
set -eu
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/.."

NUM_NODES=${NUM_NODES:-2}
OPS=${OPS:-3000}
DEV_PATH=${DEV_PATH:-/dev/dax0.1}
LOG=${LOG:-$SCRIPT_DIR/thread_scan_cxl.log}

> "$LOG"

for OPT in A B C; do
    for T in 1 2 4 8; do
        # A is too slow for 3000 ops w/ 50% write → reduce
        if [[ "$OPT" == "A" ]]; then
            OPS_USE=500
        else
            OPS_USE=$OPS
        fi
        echo "=== opt=$OPT threads=$T ops=$OPS_USE ==="
        PIDS=()
        for (( NID=0; NID<NUM_NODES; NID++ )); do
            if [[ $NID -eq 0 ]]; then SLEEP=0; else SLEEP=0.3; fi
            (
                sleep "$SLEEP"
                ./bench/ycsb_abc_bench \
                    --opt="$OPT" --workload=A \
                    --nodes=$NUM_NODES --node-id=$NID \
                    --threads=$T --ops=$OPS_USE \
                    --path="$DEV_PATH"
            ) >> "$LOG" 2>&1 &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" || true; done
        sleep 1
    done
done
echo "Done. Log: $LOG"
