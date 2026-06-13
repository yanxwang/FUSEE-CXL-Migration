#!/usr/bin/env bash
# iter-20A Exp 2: 4-path stage decomp T-sweep on cleaned-allprobe build.
# Mirrors iter-16A_xhost_decomp_sweep params. All 4 paths in one build (all probes on).

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter20A_exp2_decomp_${TS}
mkdir -p $OUT/raw $OUT/cells

H0=g1; H1=g2; DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072
TRACE=/tmp/microbench_traces
BUILD=cleaned-allprobe
KD=zipf-0.99
T_LIST="${1:-1 2 4 8 16 32 64}"

# scenario → analyzer mapping
declare -A ANALYZERS=(
  [xhost_write]="iter16A_xhost_decomp_analyze.py"
  [xhost_read]="iter18A_read_decomp_analyze.py"
  [local_read]="iter19A_local_read_decomp_analyze.py"
  [local_write]="iter20A_local_write_decomp_analyze.py"
)

# Push analyzers to g1+g2 so we can run on-host
for H in $H0 $H1; do
  ssh $H 'mkdir -p /tmp/probes_analyzers' &
done
wait
for k in "${!ANALYZERS[@]}"; do
  for H in $H0 $H1; do
    scp $ROOT/scripts/${ANALYZERS[$k]} $H:/tmp/probes_analyzers/ >/dev/null 2>&1 &
  done
done
wait

run_cell() {
  local sc=$1 T=$2 rep=$3
  local id="${sc}_T${T}_rep${rep}"
  local ck=$RANDOM$$
  local bd=/root/FUSEE_CXL/build-cxl-w1-v1024-${BUILD}
  local PROBE_DIR=/tmp/probes_${sc}_T${T}_rep${rep}
  for h in $H0 $H1; do
    ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null; rm -rf $PROBE_DIR; mkdir -p $PROBE_DIR" >/dev/null 2>&1
  done
  sleep 1

  local t0=$(date +%s)
  ssh $H1 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=${sc} FUSEE_PROBE_DUMP=$PROBE_DIR/probe \
    timeout 240 ./tests/protocol_a_ycsb $DEV ${TRACE}/bench_${sc}_${KD}_h1.spec_load ${TRACE}/bench_${sc}_${KD}_h1.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  PH1=$!
  ssh $H0 "cd $bd && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$ck FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=${sc} FUSEE_PROBE_DUMP=$PROBE_DIR/probe \
    timeout 240 ./tests/protocol_a_ycsb $DEV ${TRACE}/bench_${sc}_${KD}_h0.spec_load ${TRACE}/bench_${sc}_${KD}_h0.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1
  wait $PH1 2>/dev/null
  local t1=$(date +%s)
  local wc=$((t1-t0))

  # Truncate probe files to actual frame count (avoid pulling back 128 MB empties)
  for h in $H0 $H1; do
    ssh $h "for f in $PROBE_DIR/probe.*; do
      [ -f \"\$f\" ] || continue
      count=\$(python3 -c \"import struct; d=open('\$f','rb').read(16); print(struct.unpack('<Q',d[8:16])[0])\" 2>/dev/null)
      [ -n \"\$count\" ] && truncate -s \$((16 + count * 24)) \"\$f\"
    done" >/dev/null 2>&1 &
  done
  wait

  # Run analyzer on-host (avoid pulling 8 GB / cell)
  local analyzer=${ANALYZERS[$sc]}
  local cell_dir=$OUT/cells/${id}
  mkdir -p $cell_dir
  ssh $H0 "python3 /tmp/probes_analyzers/${analyzer} $PROBE_DIR --cpu-ghz 2.4 2>/dev/null" > $cell_dir/h0_decomp.txt 2>&1
  ssh $H1 "python3 /tmp/probes_analyzers/${analyzer} $PROBE_DIR --cpu-ghz 2.4 2>/dev/null" > $cell_dir/h1_decomp.txt 2>&1
  # Cleanup remote probes
  for h in $H0 $H1; do ssh $h "rm -rf $PROBE_DIR" >/dev/null 2>&1 & done
  wait

  # thpt from raw
  local thpt=$(grep -oP "^YCSB.*trans_agg_thpt=\K[\d.]+" $OUT/raw/${id}_h0.out | head -1)
  local th1=$(grep -oP "^YCSB.*trans_agg_thpt=\K[\d.]+" $OUT/raw/${id}_h1.out | head -1)
  local tmops=$(awk -v a=${thpt:-0} -v b=${th1:-0} 'BEGIN{printf "%.3f",(a+b)/1e6}')
  echo "[$id] thpt=$tmops Mops wc=${wc}s decomp→ $cell_dir/"
}

echo "=== Exp 2 4-path decomp T-sweep @ $(date) ==="
for sc in xhost_write xhost_read local_read local_write; do
  for T in $T_LIST; do
    for rep in 1 2 3; do
      run_cell $sc $T $rep
    done
  done
done
echo "=== sweep done @ $(date) ==="
echo "OUT: $OUT"
