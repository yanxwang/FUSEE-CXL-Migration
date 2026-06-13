#!/usr/bin/env bash
# Phase 6.0-continue (α) — perf stat on 4 cells, measure IPC
#
# Verifies the software-lock-contention hypothesis from Phase 6.0:
# - If zipf-1.5 IPC drops dramatically vs zipf-0.99, threads are spinning
#   on locks (low IPC = many cycles per useful instruction)
# - If IPC stays constant, contention is elsewhere (memory BW?)

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter15A_phase6_0b_perfstat_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
T=64
V=1024
CACHE_BUCKETS=131072  # cache=10%
BUILD="build-cxl-w1-v${V}"

run_perfstat_cell() {
  local sc="$1" kd="$2"
  local id="ph60b_${sc}_${kd//-/}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_h1.out"
  local stat_file="$OUT_BASE/raw/${id}_perfstat.txt"

  echo "[run] $id"

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    perf stat -e task-clock,cycles,instructions,branches,branch-misses,context-switches,cpu-migrations,page-faults -o /tmp/${id}_stat.txt -- bash -c '
      FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
      FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
      FUSEE_WORKLOAD_NAME=$sc FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
      ./tests/protocol_a_ycsb $DEV \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_load \
        /root/FUSEE_CXL/setup/iter15A_microbench_traces/bench_${sc}_${kd}_h0.spec_trans \
        $NUM_BUCKETS $TRANS_OPS
    ' 2>&1
    cat /tmp/${id}_stat.txt
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

  # Pull perf stat output (it's at end of out_h0 via cat above)
  cp "$out_h0" "$stat_file"

  # Parse
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  local thpt_M=$(awk -v t=${thpt:-0} 'BEGIN{printf "%.3f", t/1e6}')
  # perf stat output has "instructions  # X.XX  insn per cycle"
  local ipc=$(grep -oP 'insn per cycle' "$stat_file" | head -1 || echo "")
  local ipc_val=$(grep "insn per cycle" "$stat_file" | head -1 | grep -oP '[0-9]+\.[0-9]+' | head -1)
  [[ -z "$ipc_val" ]] && ipc_val=0
  local cs=$(grep -oP '^\s+(\d[\d,]*)\s+context-switches' "$stat_file" | head -1 | grep -oP '^\s+(\d[\d,]*)' | tr -d ' ,')
  [[ -z "$cs" ]] && cs=0
  local cyc=$(grep "cycles" "$stat_file" | head -1 | grep -oP '^\s+(\d[\d,]*)' | tr -d ' ,')
  [[ -z "$cyc" ]] && cyc=0
  local ins=$(grep "instructions" "$stat_file" | head -1 | grep -oP '^\s+(\d[\d,]*)' | tr -d ' ,')
  [[ -z "$ins" ]] && ins=0

  echo "  [done] $id thpt=$thpt_M Mops IPC=$ipc_val cycles=$cyc instr=$ins ctx-sw=$cs"
  echo "$id,$sc,$kd,$thpt_M,$ipc_val,$cyc,$ins,$cs" >> "$OUT_BASE/summary.csv"
}

echo "id,scenario,keydist,thpt_Mops,IPC,cycles,instructions,context_switches" > "$OUT_BASE/summary.csv"

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2; exit 1; fi
done

for cell in "local_read zipf-0.99" "local_read zipf-1.5" "local_write zipf-0.99" "local_write zipf-1.5"; do
  read -r sc kd <<< "$cell"
  run_perfstat_cell "$sc" "$kd"
  sleep 2
done

echo "==done=="
cat "$OUT_BASE/summary.csv"
