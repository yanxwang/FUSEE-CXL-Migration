#!/usr/bin/env bash
# iter-16A xhost receiver-NOOP study
# Per docs/microbench_xhost_spec.md §D
#
# RN-A (write): 4 levels × 7 T × xhost_write × 3 reps = 84 runs
# RN-B (read):  3 levels × 7 T × xhost_read × 3 reps  = 63 runs (L1=L0 not retested)
# Total: 147 runs ≈ 70 min

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter16A_xhost_rn_study_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072  # cache=10%
BUILD="build-cxl-w1-v${V}"
KD="zipf-0.99"
TS_LIST="1 2 4 8 16 32 64"

CSV="$OUT_BASE/grid.csv"
echo "scenario,noop_level,T,rep,thpt_Mops,r_p50_us,r_p99_us,w_p50_us,w_p99_us,wallclock_s" > "$CSV"

run_cell() {
  local sc="$1" level="$2" T="$3" rep="$4"
  local id="ph_rn_${sc}_L${level}_T${T}"
  local cookie=$RANDOM$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.out"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    FUSEE_RECEIVER_NOOP_LEVEL=$level \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h0.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    FUSEE_RECEIVER_NOOP_LEVEL=$level \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)
  local wall=$((t_end - t_start))

  local thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1); [[ -z "$thpt0" ]] && thpt0=0
  local thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1); [[ -z "$thpt1" ]] && thpt1=0
  local thpt_M=$(awk -v a=$thpt0 -v b=$thpt1 'BEGIN{printf "%.3f", (a+b)/1e6}')
  local r_p50_ns=$(grep -oP 'r_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p50_ns" ]] && r_p50_ns=0
  local r_p99_ns=$(grep -oP 'r_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p99_ns" ]] && r_p99_ns=0
  local w_p50_ns=$(grep -oP 'w_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p50_ns" ]] && w_p50_ns=0
  local w_p99_ns=$(grep -oP 'w_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p99_ns" ]] && w_p99_ns=0
  local r_p50=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local w_p50=$(awk -v n=$w_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local w_p99=$(awk -v n=$w_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')

  echo "$sc,$level,$T,$rep,$thpt_M,$r_p50,$r_p99,$w_p50,$w_p99,$wall" >> "$CSV"
  echo "  [done] $sc L=$level T=$T rep=$rep thpt=$thpt_M Mops wall=${wall}s"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n procs, aborting" >&2; exit 1; fi
done

# RN-A: write × 4 levels
echo "=== RN-A: xhost_write × 4 levels × 7 T × 3 reps = 84 runs ==="
for level in 0 1 2 3; do
  for T in $TS_LIST; do
    for rep in 1 2 3; do
      run_cell xhost_write $level $T $rep
      sleep 1
    done
  done
done

# RN-B: read × 3 levels (L1=L0 not retested)
echo "=== RN-B: xhost_read × 3 levels × 7 T × 3 reps = 63 runs ==="
for level in 0 2 3; do
  for T in $TS_LIST; do
    for rep in 1 2 3; do
      run_cell xhost_read $level $T $rep
      sleep 1
    done
  done
done

echo "==done=="
echo "OUT: $OUT_BASE"
echo "Total rows: $(grep -c '^' $CSV)"
