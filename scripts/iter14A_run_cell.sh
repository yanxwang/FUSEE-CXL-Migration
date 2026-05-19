#!/usr/bin/env bash
# iter-14A — run a single (build, workload, T, kv, cache, reps) cell on g3+g4.
# Outputs SUMMARY.log lines (YCSB format) + per-rep .out files.
# Usage:
#   bash iter14A_run_cell.sh <output_dir> <build_subdir> <workload> <T> <kv> <cache> <reps> [<max_ops>]
# Example:
#   bash iter14A_run_cell.sh /tmp/m1 build-cxl-p2 workloada 64 1024 off 5 200000

set -u
OUTDIR="${1:?usage: $0 <output_dir> <build_subdir> <workload> <T> <kv> <cache> <reps> [<max_ops>]}"
BUILD="${2:?build subdir}"
WL="${3:?workload}"
T="${4:?T}"
KV="${5:?KV}"
CACHE="${6:?cache on|off}"
REPS="${7:?reps}"
MAX_OPS="${8:-200000}"
TIMEOUT_S="${TIMEOUT_S:-600}"
NUM_BUCKETS="${NUM_BUCKETS:-65536}"
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads
BIN=/root/FUSEE_CXL/$BUILD/tests/protocol_a_ycsb

mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.log"
: > "$SUMMARY"

cache_flag=0; [ "$CACHE" = "on" ] && cache_flag=1
tag="${BUILD}_${WL}_T${T}_kv${KV}_${CACHE}"

for rep in $(seq 1 $REPS); do
  cookie=$(date +%s%N)
  ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
  ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
  wait
  sleep 0.2
  cmd_h0="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  cmd_h1="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  ssh g3 "$cmd_h0" > "$OUTDIR/${tag}_rep${rep}_h0.out" 2>"$OUTDIR/${tag}_rep${rep}_h0.err" &
  ssh g4 "$cmd_h1" > "$OUTDIR/${tag}_rep${rep}_h1.out" 2>"$OUTDIR/${tag}_rep${rep}_h1.err" &
  wait
  if [ -s "$OUTDIR/${tag}_rep${rep}_h0.out" ]; then
    cat "$OUTDIR/${tag}_rep${rep}_h0.out" >> "$SUMMARY"
  else
    echo "# FAIL: ${tag}_rep${rep} no output" >> "$SUMMARY"
  fi
done

# Extract trans_agg_thpt values for quick scan
echo "--- $tag medians ---"
grep -E "^YCSB" "$SUMMARY" | awk '{
  for(i=1;i<=NF;i++){
    if(match($i, "trans_agg_thpt=")){print substr($i, 16); break}
  }
}' | sort -n
