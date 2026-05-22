#!/usr/bin/env bash
# iter-16A xhost_write T-sweep — canonical defaults per microbench_xhost_spec.md.
#
# Params (all match iter-15A Phase 2):
#   V=1024, NUM_BUCKETS=8388608, cache_buckets=131072 (10%), zipf-0.99,
#   trans_ops=5000000, T ∈ {1,2,4,8,16,32,64}, 3 reps × xhost_write = 21 runs.
#
# Output:
#   docs/iter16A_xhost_T_sweep_<ts>/{grid.csv, raw/*, plot, summary_table}

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter16A_xhost_T_sweep_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072  # 10% of NUM_BUCKETS
BUILD="build-cxl-w1-v${V}"
KD="zipf-0.99"
SCEN="xhost_write"
TS_LIST="1 2 4 8 16 32 64"
TRACE=/root/FUSEE_CXL/setup/iter15A_microbench_traces

CSV="$OUT_BASE/grid.csv"
echo "scenario,T,rep,thpt_Mops,r_p50_us,r_p99_us,w_p50_us,w_p99_us,wallclock_s" > "$CSV"

run_cell() {
  local T="$1" rep="$2"
  local id="ph_T${T}_${SCEN}"
  local cookie=$RANDOM$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.out"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)
  local wall=$((t_end - t_start))

  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local thpt_M=$(awk -v a=$thpt 'BEGIN{printf "%.3f", a/1e6}')
  local r_p50_ns=$(grep -oP 'r_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p50_ns" ]] && r_p50_ns=0
  local r_p99_ns=$(grep -oP 'r_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p99_ns" ]] && r_p99_ns=0
  local w_p50_ns=$(grep -oP 'w_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p50_ns" ]] && w_p50_ns=0
  local w_p99_ns=$(grep -oP 'w_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p99_ns" ]] && w_p99_ns=0
  local r_p50=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local w_p50=$(awk -v n=$w_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local w_p99=$(awk -v n=$w_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')

  echo "$SCEN,$T,$rep,$thpt_M,$r_p50,$r_p99,$w_p50,$w_p99,$wall" >> "$CSV"
  echo "  [done] T=$T rep=$rep thpt=$thpt_M Mops wall=${wall}s"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n procs, aborting" >&2; exit 1; fi
done

echo "=== xhost_write T-sweep: 7 T × 3 reps = 21 runs ==="
for T in $TS_LIST; do
  for rep in 1 2 3; do
    run_cell $T $rep
    sleep 1
  done
done

echo "==done=="
echo "OUT: $OUT_BASE"
echo "Total rows: $(grep -c '^' $CSV)"
