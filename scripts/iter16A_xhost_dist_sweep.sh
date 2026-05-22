#!/usr/bin/env bash
# iter-16A xhost_write key-distribution sweep (probe-on) — varies keydist
# at fixed V/T. dist ∈ {uniform, zipf-0.5, zipf-0.99, zipf-1.5} × T ∈ {1,8,64} × 3 = 36 cells.
#
# Goal: see how Stage 7 (rcv_work, contains bucket lock) scales with key
# distribution skew — provides data for multi-receiver hot-bucket design.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter16A_xhost_dist_sweep_${TS}"
mkdir -p "$OUT_BASE/cells"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072
BUILD="build-cxl-w1-v${V}-probe"
SCEN="xhost_write"
DIST_LIST="uniform zipf-0.5 zipf-0.99 zipf-1.5"
TS_LIST="1 8 64"
TRACE=/root/FUSEE_CXL/setup/iter15A_microbench_traces

CSV="$OUT_BASE/aggregate.csv"
echo "keydist,T,rep,n_paired_ops,stage1_p50_ns,stage1_p99_ns,stage2_p50_ns,stage2_p99_ns,stage3_p50_ns,stage3_p99_ns,stage4_p50_ns,stage4_p99_ns,stage5_p50_ns,stage5_p99_ns,stagew_p50_ns,stagew_p99_ns,stage6_p50_ns,stage6_p99_ns,stage7_p50_ns,stage7_p99_ns,stage8_p50_ns,stage8_p99_ns,stager_p50_ns,stager_p99_ns,rtt_p50_ns,rtt_p99_ns,xws2r_rate_pct,xws5t_rate_pct,gap_rate_pct,thpt_Mops" > "$CSV"

run_cell() {
  local KD="$1" T="$2" rep="$3"
  local cell_id="dist${KD}_T${T}_rep${rep}"
  local cell_out="$OUT_BASE/cells/$cell_id"
  mkdir -p "$cell_out/probes_h0" "$cell_out/probes_h1"
  local cookie=$RANDOM$RANDOM
  local t_start=$(date +%s)

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

  rsync -a root@g3:/tmp/probes/ "$cell_out/probes_h0/" >/dev/null 2>&1
  rsync -a root@g4:/tmp/probes/ "$cell_out/probes_h1/" >/dev/null 2>&1

  python3 "$ROOT/scripts/iter16A_xhost_decomp_analyze.py" \
    "$cell_out/probes_h0" "$cell_out/probes_h1" --cpu-ghz 2.4 \
    --out "$cell_out/per_op.csv" > "$cell_out/summary.txt" 2>&1

  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$cell_out/h0.out" | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local thpt_M=$(awk -v a=$thpt 'BEGIN{printf "%.3f", a/1e6}')

  python3 - <<EOF >> "$CSV"
import re
with open("$cell_out/summary.txt") as f: txt = f.read()
metrics = {}
for line in txt.splitlines():
    m = re.match(r'^(Stage\d|StageW|StageR|RTT)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line)
    if m:
        metrics[m.group(1)] = {"n": int(m.group(2)), "p50_ns": float(m.group(4)), "p99_ns": float(m.group(5))}
xws2r = re.search(r'XWS2R rate = ([\d.]+)%', txt)
xws5t = re.search(r'XWS5T \(timeout\) rate = ([\d.]+)%', txt)
gap = re.search(r'gap encounter rate = ([\d.]+)%', txt)
cols = ["$KD", "$T", "$rep", str(metrics.get("Stage1", {}).get("n", 0))]
for k in ["Stage1", "Stage2", "Stage3", "Stage4", "Stage5", "StageW",
          "Stage6", "Stage7", "Stage8", "StageR", "RTT"]:
    m = metrics.get(k, {})
    cols.append(f'{m.get("p50_ns", 0):.1f}')
    cols.append(f'{m.get("p99_ns", 0):.1f}')
cols.append(f'{float(xws2r.group(1)) if xws2r else 0:.4f}')
cols.append(f'{float(xws5t.group(1)) if xws5t else 0:.4f}')
cols.append(f'{float(gap.group(1)) if gap else 0:.4f}')
cols.append("$thpt_M")
print(",".join(cols))
EOF

  local wall=$((t_end - t_start))
  echo "  [done] dist=$KD T=$T rep=$rep thpt=$thpt_M Mops wall=${wall}s"
}

echo "[dist-sweep] dist_list='$DIST_LIST' T_list=$TS_LIST reps=3"
for KD in $DIST_LIST; do
  for T in $TS_LIST; do
    for rep in 1 2 3; do
      run_cell $KD $T $rep
      sleep 1
    done
  done
done

echo "==done=="
echo "OUT: $OUT_BASE"
echo "rows: $(grep -c '^' $CSV)"
