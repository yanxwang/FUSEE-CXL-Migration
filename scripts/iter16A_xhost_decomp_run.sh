#!/usr/bin/env bash
# iter-16A xhost write decomp run driver.
#
# Usage:
#   scripts/iter16A_xhost_decomp_run.sh <T> [<trans_ops>] [<rep>]
#
# Defaults: T=8, trans_ops=1000000, rep=1 (single rep — analysis is per-op).
#
# Requires:
#   - build-cxl-w1-v1024-probe (FUSEE_PROBE=1) on g3 + g4
#   - traces at /root/FUSEE_CXL/setup/iter15A_microbench_traces/
#
# Output: docs/iter16A_xhost_decomp_<T>_<ts>/
#   raw/{h0,h1}.out — stdout
#   probes_h0/probe.<pid>.<tid>  ← worker XWS* + receiver XWR* events
#   probes_h1/probe.<pid>.<tid>
#   summary.txt — analyzer output
#   per_op.csv — per-op stage latencies

set -u
T=${1:-8}
TRANS_OPS=${2:-1000000}
REP=${3:-1}

ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter16A_xhost_decomp_T${T}_${TS}
mkdir -p $OUT/raw $OUT/probes_h0 $OUT/probes_h1

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
V=1024
CACHE_BUCKETS=131072  # 10% cache
BUILD="build-cxl-w1-v1024-probe"
KD="zipf-0.99"
TRACE=/root/FUSEE_CXL/setup/iter15A_microbench_traces

echo "[decomp] T=$T trans_ops=$TRANS_OPS rep=$REP → $OUT"

# Preflight: kill any stragglers + clear remote probe dirs.
for h in g3 g4; do
  ssh root@$h "pkill -9 -f protocol_a_ycsb 2>/dev/null; rm -rf /tmp/probes && mkdir -p /tmp/probes"
done

COOKIE=$RANDOM$RANDOM
ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
  FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
  FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$REP FUSEE_CACHE=1 \
  FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
  FUSEE_PROBE_DUMP=/tmp/probes/probe \
  ./tests/protocol_a_ycsb $DEV \
    $TRACE/bench_xhost_write_${KD}_h0.spec_load \
    $TRACE/bench_xhost_write_${KD}_h0.spec_trans \
    $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/h0.out 2>&1 &
G3=$!
ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
  FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
  FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$REP FUSEE_CACHE=1 \
  FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
  FUSEE_PROBE_DUMP=/tmp/probes/probe \
  ./tests/protocol_a_ycsb $DEV \
    $TRACE/bench_xhost_write_${KD}_h1.spec_load \
    $TRACE/bench_xhost_write_${KD}_h1.spec_trans \
    $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/h1.out 2>&1 &
G4=$!
wait $G3 $G4

THPT=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/h0.out | head -1)
echo "[decomp] thpt = $THPT ops/s (probe-instrumented, slower than probe-off)"

# Pull probe files back. Use bzip-stream-like rsync.
echo "[decomp] rsyncing probe files back..."
rsync -a root@g3:/tmp/probes/ $OUT/probes_h0/ 2>&1 | tail -2
rsync -a root@g4:/tmp/probes/ $OUT/probes_h1/ 2>&1 | tail -2

echo "[decomp] analyzing..."
python3 $ROOT/scripts/iter16A_xhost_decomp_analyze.py \
  $OUT/probes_h0 $OUT/probes_h1 --cpu-ghz 2.4 \
  --out $OUT/per_op.csv > $OUT/summary.txt 2>&1
cat $OUT/summary.txt

echo ""
echo "[decomp] OUT: $OUT"
