#!/usr/bin/env bash
# iter-15A Phase 1 salvage + top-up:
# - Copies Phase 1 v2 grid.csv but filters out 3 fast-mode (bug) rows
# - Runs 5 top-up cells with fix-applied binary
# - Appends results to a new salvaged CSV
#
# Output goes to docs/iter15A_microbench_phase1_salvaged_<ts>/

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
PHASE="phase1_salvaged"
OUT_BASE="$ROOT/docs/iter15A_microbench_${PHASE}_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
CACHE_BUCKETS=131072  # cache=10%
T=64

CSV="$OUT_BASE/grid.csv"

# 1) Copy Phase 1 v2 grid.csv header
PH1_OLD=$(ls -dt $ROOT/docs/iter15A_microbench_phase1_*/ 2>/dev/null | head -1)
head -1 "$PH1_OLD/grid.csv" > "$CSV"

# 2) Filter out fast-mode rows (3 specific ones)
# Discarded: V=64 lw rep 2; V=256 lw rep 1, 2
awk -F, '
NR==1 {next}  # skip header
{
  V=$3; sc=$7; rep=$9;
  # Filter out bimodal-bug rows
  if (V=="64"   && sc=="local_write" && rep=="2") next;
  if (V=="256"  && sc=="local_write" && rep=="1") next;
  if (V=="256"  && sc=="local_write" && rep=="2") next;
  print;
}' "$PH1_OLD/grid.csv" >> "$CSV"

echo "salvaged $(wc -l < $CSV) rows (header + kept rows) into $CSV"

# 3) Top-up runs: 5 cells
# Format: V scenario rep_label (rep label collides with old reps; use 4, 5, etc.)
TOPUPS=(
  "64   local_write  4"
  "256  local_write  4"
  "256  local_write  5"
  "1024 xhost_write  2"
  "1024 xhost_write  3"
)

run_topup() {
  local V="$1" sc="$2" rep="$3"
  local kd="zipf-0.99"
  local build="build-cxl-w1-v${V}"
  local cookie=$RANDOM
  local id="ph1_V${V}_T${T}_c10_${sc}_${kd}"
  local out_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.out"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)
  local wallclock=$((t_end - t_start))

  # Parse
  local thpt0 thpt1
  thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1); [[ -z "$thpt0" ]] && thpt0=0
  thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h1" | head -1); [[ -z "$thpt1" ]] && thpt1=0
  local thpt_M
  thpt_M=$(awk -v a=$thpt0 -v b=$thpt1 'BEGIN{printf "%.3f", (a+b)/1e6}')

  local r_p50_ns r_p99_ns w_p50_ns w_p99_ns
  r_p50_ns=$(grep -oP 'r_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p50_ns" ]] && r_p50_ns=0
  r_p99_ns=$(grep -oP 'r_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p99_ns" ]] && r_p99_ns=0
  w_p50_ns=$(grep -oP 'w_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p50_ns" ]] && w_p50_ns=0
  w_p99_ns=$(grep -oP 'w_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p99_ns" ]] && w_p99_ns=0
  local r_p50 r_p99 w_p50 w_p99
  r_p50=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  r_p99=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  w_p50=$(awk -v n=$w_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  w_p99=$(awk -v n=$w_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')

  # Path counters
  declare -A pc
  for k in r0_tls r2hit r2miss_local r3 cpool_ins_r local_w fwd_w local_w_blk_fwd local_w_stg_fwd cpool_ins_w cp_evict cp_set_stale cp_lru_evict rh_served; do
    local v0 v1
    # take "after_TRANS" AGG (= total)
    v0=$(grep -oP "^# PATH host=0 label=after_TRANS AGG .*${k}=\K[0-9]+" "$out_h0" | head -1); [[ -z "$v0" ]] && v0=0
    v1=$(grep -oP "^# PATH host=1 label=after_TRANS AGG .*${k}=\K[0-9]+" "$out_h1" | head -1); [[ -z "$v1" ]] && v1=0
    pc[$k]=$((v0 + v1))
  done

  local cache_filled=0 cache_fill_pct=0
  local cf=$(grep "^# CACHE_FILL host=0" "$out_h0" | tail -1)
  if [[ -n "$cf" ]]; then
    cache_filled=$(echo "$cf" | grep -oP 'filled=\K[0-9]+'); [[ -z "$cache_filled" ]] && cache_filled=0
    cache_fill_pct=$(echo "$cf" | grep -oP 'fill_pct=\K[0-9.]+'); [[ -z "$cache_fill_pct" ]] && cache_fill_pct=0
  fi

  echo "$id,phase1,$V,$T,10,$CACHE_BUCKETS,$sc,$kd,$rep,$thpt_M,$r_p50,$r_p99,$w_p50,$w_p99,${pc[r0_tls]},${pc[r2hit]},${pc[r2miss_local]},${pc[r3]},${pc[cpool_ins_r]},${pc[local_w]},${pc[fwd_w]},${pc[local_w_blk_fwd]},${pc[local_w_stg_fwd]},${pc[cpool_ins_w]},${pc[cp_evict]},${pc[cp_set_stale]},${pc[cp_lru_evict]},${pc[rh_served]},$cache_filled,$cache_fill_pct,$wallclock" >> "$CSV"
  echo "  [topup] $id rep=$rep thpt=$thpt_M Mops wallclock=${wallclock}s"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

for spec in "${TOPUPS[@]}"; do
  read -r V sc rep <<< "$spec"
  echo "[run] $V $sc rep=$rep"
  run_topup "$V" "$sc" "$rep"
  sleep 1
done

echo "==done=="
echo "Final CSV: $CSV"
echo "Rows: $(wc -l < $CSV)"
