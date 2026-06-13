#!/usr/bin/env bash
# Phase 6.0 — perf c2c on distribution skew RCA
#
# Goal: Identify which cachelines suffer most MESI ping-pong (HITM events)
# under zipf-1.5 vs zipf-0.99 for local_read and local_write paths.
# This isolates whether zipf-1.5's local thpt collapse is driven by:
#   - cache_pool entry MESI (KvCacheEntry 1088B → 17 cachelines)
#   - bucket spinlock / slot_directory_entry MESI (LFM)
#   - hashtable bucket cacheline contention

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter15A_phase6_0_dist_c2c_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
CACHE_BUCKETS=131072  # cache=10%
T=64
V=1024
BUILD="build-cxl-w1-v${V}"

run_perfc2c_cell() {
  local sc="$1" kd="$2"
  local id="ph60_${sc}_${kd//-/}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_h1.out"
  local c2c_data="$OUT_BASE/raw/${id}_perf.data"
  local c2c_report="$OUT_BASE/raw/${id}_c2c_report.txt"

  echo "[run] $id"

  # host 0 with perf c2c record
  # Note: perf c2c needs root + a permissive paranoid setting. We use
  # sudo or direct ssh root.
  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    perf c2c record -o /tmp/${id}_perf.data -- bash -c '
      FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
      FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
      FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
      ./tests/protocol_a_ycsb $DEV \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_load \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_trans \
        $NUM_BUCKETS $TRANS_OPS
    ' 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait

  # Pull perf data + generate report on g3
  ssh root@g3 "
    perf c2c report -i /tmp/${id}_perf.data --stdio --full-symbols > /tmp/${id}_report.txt 2>&1
    head -200 /tmp/${id}_report.txt
  " > "$c2c_report" 2>&1

  # Copy perf.data to local
  scp root@g3:/tmp/${id}_perf.data "$c2c_data" 2>/dev/null || true

  # Extract thpt
  local thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  local thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1)
  local thpt_M=$(awk -v a=${thpt0:-0} -v b=${thpt1:-0} 'BEGIN{printf "%.3f", (a+b)/1e6}')

  # Extract HITM summary from c2c report (typically near top of output)
  local hitm_total=$(grep -oP 'Total HITM[^0-9]*\K[0-9]+' "$c2c_report" | head -1)
  [[ -z "$hitm_total" ]] && hitm_total=0

  echo "  [done] $id thpt=$thpt_M Mops total_HITM=$hitm_total"
  echo "$id,$sc,$kd,$thpt_M,$hitm_total" >> "$OUT_BASE/summary.csv"
}

echo "id,scenario,keydist,thpt_Mops,total_HITM" > "$OUT_BASE/summary.csv"

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

# Pre-flight: verify perf c2c works on host 0
ssh root@g3 'which perf && perf --version && cat /proc/sys/kernel/perf_event_paranoid' || {
  echo "[ERR] perf c2c prereq check failed on g3" >&2; exit 2;
}

# Run 4 cells
run_perfc2c_cell "local_read"  "zipf-0.99"
sleep 2
run_perfc2c_cell "local_read"  "zipf-1.5"
sleep 2
run_perfc2c_cell "local_write" "zipf-0.99"
sleep 2
run_perfc2c_cell "local_write" "zipf-1.5"

echo "==done=="
echo "OUT_BASE: $OUT_BASE"
echo "Summary CSV: $OUT_BASE/summary.csv"
cat "$OUT_BASE/summary.csv"
