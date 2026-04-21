#!/usr/bin/env bash
# Scan write ratio on real CXL - all 3 options
set -eu
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/.."

NUM_NODES=${NUM_NODES:-4}
THREADS=${THREADS:-2}
OPS=${OPS:-3000}
DEV_PATH=${DEV_PATH:-/dev/dax0.0}
LOG=${LOG:-$SCRIPT_DIR/wr_scan_cxl.log}

> "$LOG"

# Run Option A only for low write ratios (since it's very slow otherwise)
for OPT in A B C; do
    for WR in 0.0 0.1 0.25 0.5 0.75 1.0; do
        # skip slow combos for A
        if [[ "$OPT" == "A" && ($(echo "$WR > 0.1" | bc -l) -eq 1) && $OPS -gt 1000 ]]; then
            OPS_USE=500   # smaller OPS for A with write-heavy workload
        else
            OPS_USE=$OPS
        fi
        echo "=== opt=$OPT write_ratio=$WR ops=$OPS_USE ==="
        PIDS=()
        for (( NID=0; NID<NUM_NODES; NID++ )); do
            if [[ $NID -eq 0 ]]; then SLEEP=0; else SLEEP=0.3; fi
            (
                sleep "$SLEEP"
                ./bench/ycsb_abc_bench \
                    --opt="$OPT" --write-ratio=$WR \
                    --nodes=$NUM_NODES --node-id=$NID \
                    --threads=$THREADS --ops=$OPS_USE \
                    --path="$DEV_PATH"
            ) >> "$LOG" 2>&1 &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" || true; done
        sleep 1
    done
done
echo "Done. Log: $LOG"
