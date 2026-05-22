#!/usr/bin/env bash
# iter-16A xhost_write probe-on T-sweep — same params as probe-off T-sweep.
#
# Per cell: run probe-instrumented benchmark, truncate probe files on host,
# rsync back, run analyzer locally → per_op.csv + summary.txt per cell.
#
# Usage: iter16A_xhost_decomp_sweep.sh [<T_list>]
#   default T_list = "1 8 64" (sanity); full = "1 2 4 8 16 32 64"
#
# Param parity with probe-off sweep:
#   V=1024, NUM_BUCKETS=8388608, cache_buckets=131072, zipf-0.99,
#   trans_ops=5000000, 3 reps per T cell.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter16A_xhost_decomp_sweep_${TS}"
mkdir -p "$OUT_BASE/raw" "$OUT_BASE/cells"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072
BUILD="build-cxl-w1-v${V}-probe"
KD="zipf-0.99"
SCEN="xhost_write"
TS_LIST="${1:-1 8 64}"
TRACE=/root/FUSEE_CXL/setup/iter15A_microbench_traces

echo "[decomp-sweep] T_list='$TS_LIST', reps=3, trans_ops=$TRANS_OPS"
echo "[decomp-sweep] OUT: $OUT_BASE"

CSV="$OUT_BASE/aggregate.csv"
echo "T,rep,n_paired_ops,stage1_p50_ns,stage1_p99_ns,stage2_p50_ns,stage2_p99_ns,stage3_p50_ns,stage3_p99_ns,stage4_p50_ns,stage4_p99_ns,stage5_p50_ns,stage5_p99_ns,stagew_p50_ns,stagew_p99_ns,stage6_p50_ns,stage6_p99_ns,stage7_p50_ns,stage7_p99_ns,stage8_p50_ns,stage8_p99_ns,stager_p50_ns,stager_p99_ns,rtt_p50_ns,rtt_p99_ns,xws2r_rate_pct,xws5t_rate_pct,gap_rate_pct,thpt_Mops" > "$CSV"

run_cell() {
  local T="$1" rep="$2"
  local cell_id="T${T}_rep${rep}"
  local cell_out="$OUT_BASE/cells/$cell_id"
  mkdir -p "$cell_out/probes_h0" "$cell_out/probes_h1"
  local cookie=$RANDOM$RANDOM
  local t_start=$(date +%s)

  # Clear remote probe dirs.
  for h in g3 g4; do
    ssh root@$h "rm -rf /tmp/probes && mkdir -p /tmp/probes" >/dev/null 2>&1
  done

  ssh root@g3 "cd /root/FUSEE_CXL/${BUILD} && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    FUSEE_PROBE_DUMP=/tmp/probes/probe \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > "$cell_out/h0.out" 2>&1 &
  ssh root@g4 "cd /root/FUSEE_CXL/${BUILD} && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    FUSEE_PROBE_DUMP=/tmp/probes/probe \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > "$cell_out/h1.out" 2>&1 &
  wait
  local t_end=$(date +%s)

  # Truncate probe files on each host to actual data size (header + count*24).
  # Default mmap is 128MB but actual data may be much smaller.
  for h in g3 g4; do
    ssh root@$h '
      for f in /tmp/probes/probe.*; do
        if [ -f "$f" ]; then
          count=$(python3 -c "import struct; print(struct.unpack(\"<Q\", open(\"$f\",\"rb\").read()[8:16])[0])")
          size=$((16 + count * 24))
          truncate -s $size "$f"
        fi
      done
    ' >/dev/null 2>&1
  done

  # Rsync back.
  rsync -a root@g3:/tmp/probes/ "$cell_out/probes_h0/" >/dev/null 2>&1
  rsync -a root@g4:/tmp/probes/ "$cell_out/probes_h1/" >/dev/null 2>&1

  # Run analyzer locally.
  python3 "$ROOT/scripts/iter16A_xhost_decomp_analyze.py" \
    "$cell_out/probes_h0" "$cell_out/probes_h1" --cpu-ghz 2.4 \
    --out "$cell_out/per_op.csv" > "$cell_out/summary.txt" 2>&1

  # Extract aggregate stats for CSV row.
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$cell_out/h0.out" | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local thpt_M=$(awk -v a=$thpt 'BEGIN{printf "%.3f", a/1e6}')

  # Pull n + p50/p99 ns for each metric from summary.txt
  python3 - <<EOF >> "$CSV"
import re
with open("$cell_out/summary.txt") as f:
    txt = f.read()

# parse table rows: "metricname  n  p50_cyc  p50_ns  p99_ns  avg_ns"
metrics = {}
for line in txt.splitlines():
    m = re.match(r'^(Stage\d|StageW|StageR|RTT)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line)
    if m:
        metrics[m.group(1)] = {
            "n": int(m.group(2)),
            "p50_ns": float(m.group(4)),
            "p99_ns": float(m.group(5)),
        }

# parse counters
xws2r_rate = re.search(r'XWS2R rate = ([\d.]+)%', txt)
xws5t_rate = re.search(r'XWS5T \(timeout\) rate = ([\d.]+)%', txt)
gap_rate = re.search(r'gap encounter rate = ([\d.]+)%', txt)

cols = [
    "$T", "$rep",
    str(metrics.get("Stage1", {}).get("n", 0)),
]
for k in ["Stage1", "Stage2", "Stage3", "Stage4", "Stage5", "StageW",
          "Stage6", "Stage7", "Stage8", "StageR", "RTT"]:
    m = metrics.get(k, {})
    cols.append(f'{m.get("p50_ns", 0):.1f}')
    cols.append(f'{m.get("p99_ns", 0):.1f}')
cols.append(f'{float(xws2r_rate.group(1)) if xws2r_rate else 0:.4f}')
cols.append(f'{float(xws5t_rate.group(1)) if xws5t_rate else 0:.4f}')
cols.append(f'{float(gap_rate.group(1)) if gap_rate else 0:.4f}')
cols.append("$thpt_M")
print(",".join(cols))
EOF

  local wall=$((t_end - t_start))
  echo "  [done] T=$T rep=$rep thpt=$thpt_M Mops wall=${wall}s"
}

for T in $TS_LIST; do
  for rep in 1 2 3; do
    run_cell $T $rep
    sleep 1
  done
done

echo ""
echo "==done=="
echo "OUT: $OUT_BASE"
echo "Aggregate CSV: $CSV"
echo "  rows: $(grep -c '^' $CSV)"
