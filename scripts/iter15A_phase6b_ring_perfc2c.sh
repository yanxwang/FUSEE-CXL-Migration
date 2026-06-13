#!/usr/bin/env bash
# Phase 6b — perf c2c on xhost_write to find ring tail HITM
#
# Goal: Verify Backlog Task 4's "ring head/tail MESI ping-pong" prediction.
# Running xhost_write at T=64 with high cross-host traffic. Look for
# HITM hot cachelines that map to WriteRing.tail or related ring structures.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter15A_phase6b_ring_c2c_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
T=64
V=1024
CACHE_BUCKETS=131072
BUILD="build-cxl-w1-v${V}"

run_perfc2c_cell() {
  local sc="$1" kd="$2"
  local id="ph6b_${sc}_${kd//-/}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_h1.out"
  local c2c_report="$OUT_BASE/raw/${id}_c2c_report.txt"

  echo "[run] $id"

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

  ssh root@g3 "
    perf c2c report -i /tmp/${id}_perf.data --stdio --full-symbols
  " > "$c2c_report" 2>&1

  local thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  local thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1)
  local thpt_M=$(awk -v a=${thpt0:-0} -v b=${thpt1:-0} 'BEGIN{printf "%.3f", (a+b)/1e6}')
  local hitm=$(grep -oP 'Load Local HITM\s*:\s+\K[0-9]+' "$c2c_report" | head -1); [[ -z "$hitm" ]] && hitm=0
  local shared=$(grep -oP 'Total Shared Cache Lines\s*:\s+\K[0-9]+' "$c2c_report" | head -1); [[ -z "$shared" ]] && shared=0
  echo "  [done] $id thpt=$thpt_M Mops LL_HITM=$hitm shared_lines=$shared"
  echo "$id,$sc,$kd,$thpt_M,$hitm,$shared" >> "$OUT_BASE/summary.csv"
}

echo "id,scenario,keydist,thpt_Mops,LL_HITM,total_shared_lines" > "$OUT_BASE/summary.csv"

for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

# xhost_write zipf-0.99 (the canonical case where ring tail should be hot)
run_perfc2c_cell "xhost_write" "zipf-0.99"
# Plus local_write zipf-0.99 as control (no ring traffic, ring tail should be cold)
sleep 2
run_perfc2c_cell "local_write" "zipf-0.99"

echo "==done=="
cat "$OUT_BASE/summary.csv"
