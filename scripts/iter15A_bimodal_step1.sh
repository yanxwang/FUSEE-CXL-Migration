#!/usr/bin/env bash
# iter-15A bimodal debug Step 1 sweep:
#   V ∈ {64, 256} × T=64 × cache=10% × local_write × zipf-0.99 × 5 reps each
# Each cell dumps path counters TWICE (after_LOAD + after_TRANS) so we can
# isolate LOAD-phase from TRANS-phase activity per rep.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter15A_bimodal_step1_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
CACHE_BUCKETS=131072  # cache=10%
T=64

CSV="$OUT_BASE/bimodal.csv"
echo "V,rep,thpt_Mops,wall_load_s,wall_trans_s,load_cpool_ins_w,trans_cpool_ins_w,load_local_w,trans_local_w,load_cp_lru_evict,trans_cp_lru_evict" > "$CSV"

run_rep() {
  local V="$1" rep="$2"
  local build="build-cxl-w1-v${V}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/V${V}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/V${V}_rep${rep}_h1.out"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_write FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_local_write_zipf-0.99_h0.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_local_write_zipf-0.99_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_write FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_local_write_zipf-0.99_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_local_write_zipf-0.99_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)

  # Parse: thpt + LOAD/TRANS counters
  local thpt0 thpt1
  thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1); [[ -z "$thpt0" ]] && thpt0=0
  thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1); [[ -z "$thpt1" ]] && thpt1=0
  local thpt_M
  thpt_M=$(awk -v a=$thpt0 -v b=$thpt1 'BEGIN{printf "%.3f", (a+b)/1e6}')

  # Extract LOAD AGG counters (sum h0+h1)
  local load_cpool_ins_w=0 load_local_w=0 load_cp_lru=0
  for f in "$out_h0" "$out_h1"; do
    local line
    line=$(grep -oP "^# PATH host=. label=after_LOAD AGG .*" "$f" | tail -1)
    [[ -z "$line" ]] && continue
    local v
    v=$(echo "$line" | grep -oP "cpool_ins_w=\K[0-9]+"); load_cpool_ins_w=$((load_cpool_ins_w + ${v:-0}))
    v=$(echo "$line" | grep -oP "local_w=\K[0-9]+");    load_local_w=$((load_local_w + ${v:-0}))
    v=$(echo "$line" | grep -oP "cp_lru_evict=\K[0-9]+"); load_cp_lru=$((load_cp_lru + ${v:-0}))
  done

  # Extract TRANS AGG counters (sum h0+h1)
  local trans_cpool_ins_w_total=0 trans_local_w_total=0 trans_cp_lru_total=0
  for f in "$out_h0" "$out_h1"; do
    local line
    line=$(grep -oP "^# PATH host=. label=after_TRANS AGG .*" "$f" | tail -1)
    [[ -z "$line" ]] && continue
    local v
    v=$(echo "$line" | grep -oP "cpool_ins_w=\K[0-9]+"); trans_cpool_ins_w_total=$((trans_cpool_ins_w_total + ${v:-0}))
    v=$(echo "$line" | grep -oP "local_w=\K[0-9]+");    trans_local_w_total=$((trans_local_w_total + ${v:-0}))
    v=$(echo "$line" | grep -oP "cp_lru_evict=\K[0-9]+"); trans_cp_lru_total=$((trans_cp_lru_total + ${v:-0}))
  done

  # TRANS-only = after_TRANS - after_LOAD
  local trans_cpool_ins_w=$((trans_cpool_ins_w_total - load_cpool_ins_w))
  local trans_local_w=$((trans_local_w_total - load_local_w))
  local trans_cp_lru=$((trans_cp_lru_total - load_cp_lru))

  # Extract wall_trans from output
  local wall_trans
  wall_trans=$(grep -oP 'trans_wall_max=\K[0-9.]+' "$out_h0" | head -1)
  [[ -z "$wall_trans" ]] && wall_trans=0

  # wall_load_s = (t_end - t_start) - trans (approx; trans_wall is sec)
  local wall_total=$((t_end - t_start))
  local wall_load=$(awk -v t=$wall_total -v tr=$wall_trans 'BEGIN{printf "%.3f", t-tr}')

  echo "$V,$rep,$thpt_M,$wall_load,$wall_trans,$load_cpool_ins_w,$trans_cpool_ins_w,$load_local_w,$trans_local_w,$load_cp_lru,$trans_cp_lru" >> "$CSV"
  echo "  [V=$V rep=$rep] thpt=$thpt_M Mops  LOAD: cpool_ins_w=$load_cpool_ins_w local_w=$load_local_w cp_lru=$load_cp_lru  TRANS: cpool_ins_w=$trans_cpool_ins_w local_w=$trans_local_w cp_lru=$trans_cp_lru"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

for V in 64 256; do
  for rep in 1 2 3 4 5 6 7 8 9 10; do
    echo "[run] V=$V rep=$rep"
    run_rep "$V" "$rep"
    sleep 1
  done
done

echo "==done=="
echo "CSV: $CSV"
