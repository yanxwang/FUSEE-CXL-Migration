#!/usr/bin/env bash
# Phase 6a — perf c2c on local_read across cache% sweep
#
# Goal: Identify mechanism behind Phase 3 finding "local_read drops
# 2.5× as cache% grows 1→100" — is it cache_pool entry MESI, or
# something else (cache_pool memcpy bandwidth / LRU eviction storm)?

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter15A_phase6a_cachepct_c2c_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
T=64
V=1024
BUILD="build-cxl-w1-v${V}"
SC="local_read"
KD="zipf-0.99"

declare -A CACHE_BUCKETS_MAP=(
  [1]=16384
  [10]=131072
  [100]=2097152
)

run_perfc2c_cell() {
  local cp="$1"
  local cb=${CACHE_BUCKETS_MAP[$cp]}
  local id="ph6a_c${cp}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_h1.out"
  local c2c_report="$OUT_BASE/raw/${id}_c2c_report.txt"

  echo "[run] $id (cache%=$cp, cache_buckets=$cb)"

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    perf c2c record -o /tmp/${id}_perf.data -- bash -c '
      FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
      FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
      FUSEE_WORKLOAD_NAME=$SC FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
      ./tests/protocol_a_ycsb $DEV \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${SC}_${KD}_h0.spec_load \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${SC}_${KD}_h0.spec_trans \
        $NUM_BUCKETS $TRANS_OPS
    ' 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SC FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${SC}_${KD}_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${SC}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait

  ssh root@g3 "
    perf c2c report -i /tmp/${id}_perf.data --stdio --full-symbols
  " > "$c2c_report" 2>&1

  local thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  local thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1)
  local thpt_M=$(awk -v a=${thpt0:-0} -v b=${thpt1:-0} 'BEGIN{printf "%.3f", (a+b)/1e6}')
  local hitm=$(grep -oP 'Load Local HITM\s*:\s+\K[0-9]+' "$c2c_report" | head -1)
  [[ -z "$hitm" ]] && hitm=0
  local llc=$(grep -oP 'Load LLC hit\s*:\s+\K[0-9]+' "$c2c_report" | head -1)
  [[ -z "$llc" ]] && llc=0
  local shared=$(grep -oP 'Total Shared Cache Lines\s*:\s+\K[0-9]+' "$c2c_report" | head -1)
  [[ -z "$shared" ]] && shared=0
  echo "  [done] $id thpt=$thpt_M Mops LL_HITM=$hitm LLC_hit=$llc shared=$shared"
  echo "$id,$cp,$cb,$thpt_M,$hitm,$llc,$shared" >> "$OUT_BASE/summary.csv"
}

echo "id,cache_pct,cache_buckets,thpt_Mops,LL_HITM,LLC_hit,total_shared_lines" > "$OUT_BASE/summary.csv"

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

for cp in 1 10 100; do
  run_perfc2c_cell "$cp"
  sleep 2
done

echo "==done=="
echo "OUT_BASE: $OUT_BASE"
cat "$OUT_BASE/summary.csv"
