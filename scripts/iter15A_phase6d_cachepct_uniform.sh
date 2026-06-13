#!/usr/bin/env bash
# Phase 6d — repeat Phase 3 (cache% sweep) with uniform key distribution
# instead of zipf-0.99. Verifies whether the Phase 3 finding "local_read
# thpt drops 2.5× as cache% grows" also holds under uniform access (less
# hot-key concentration → maybe different cache hit behavior).

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
PHASE="phase6d_uniform"
OUT_BASE="$ROOT/docs/iter15A_microbench_${PHASE}_${TS}"
mkdir -p "$OUT_BASE/raw"

CSV="$OUT_BASE/grid.csv"
echo "id,phase,V,T,cache_pct,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,w_p50_us,w_p99_us,r0_tls,r2hit,r2miss_local,r3,cpool_ins_r,local_w,fwd_w,local_w_blk_fwd,local_w_stg_fwd,cpool_ins_w,cp_evict,cp_set_stale,cp_lru_evict,rh_served,cache_filled,cache_fill_pct,wallclock_s" > "$CSV"

# HR-2 thresholds.
TOTAL_TRANS_OPS_CLUSTER=5000000
HR2_W_LO=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 0.9)}')
HR2_W_HI=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 1.1)}')
HR2_R_LO=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 0.05)}')
HR2_R_HI=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 1.1)}')

declare -A CACHE_BUCKETS=(
  [1]=16384 [2]=32768 [5]=65536 [10]=131072 [20]=262144 [50]=524288 [100]=2097152
)

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
T=64
V=1024
BUILD="build-cxl-w1-v${V}"
KD="uniform"

run_cell() {
  local cp="$1" sc="$2" rep="$3"
  local cb=${CACHE_BUCKETS[$cp]}
  local id="ph6d_V${V}_T${T}_c${cp}_${sc}_${KD}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.out"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h0.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h1.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)
  local wallclock=$((t_end - t_start))

  # Parse
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

  declare -A pc
  for k in r0_tls r2hit r2miss_local r3 cpool_ins_r local_w fwd_w local_w_blk_fwd local_w_stg_fwd cpool_ins_w cp_evict cp_set_stale cp_lru_evict rh_served; do
    local v0 v1
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

  echo "$id,phase6d_uniform,$V,$T,$cp,$cb,$sc,$KD,$rep,$thpt_M,$r_p50,$r_p99,$w_p50,$w_p99,${pc[r0_tls]},${pc[r2hit]},${pc[r2miss_local]},${pc[r3]},${pc[cpool_ins_r]},${pc[local_w]},${pc[fwd_w]},${pc[local_w_blk_fwd]},${pc[local_w_stg_fwd]},${pc[cpool_ins_w]},${pc[cp_evict]},${pc[cp_set_stale]},${pc[cp_lru_evict]},${pc[rh_served]},$cache_filled,$cache_fill_pct,$wallclock" >> "$CSV"
  echo "  [done] $id rep=$rep thpt=$thpt_M Mops wall=${wallclock}s"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

# Run: cache% × 4 scenarios × 3 reps (uniform only)
for cp in 1 2 5 10 20 50 100; do
  for sc in local_read xhost_read local_write xhost_write; do
    for rep in 1 2 3; do
      run_cell "$cp" "$sc" "$rep"
      sleep 1
    done
  done
done

echo "==done=="
echo "CSV: $CSV"
