#!/usr/bin/env bash
# iter-14A P6 ground-truth microbench: 4 scenarios × 4 T × cache × keyDist × 3 reps.
# Each host uses its OWN spec_load + spec_trans (filtered by sharding).
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/MICROBENCH.tsv"
echo -e "scenario\tkeydist\tT\tcache\trep\ttrans_agg_thpt\tw_p50\tw_p99\tr_p50\tr_p99" > "$SUMMARY"

# As-shipped build (post P2 ROLLBACK + post P4 F2 ROLLBACK) = build-cxl-w1:
# HAZARD direct-pool read + W1 RESERVED direct-pool write; both
# FUSEE_XHOST_WRITE_SELF_INVAL=0 and FUSEE_LRU_SAMPLE=0 (defaults).
BUILD="${BUILD:-build-cxl-w1}"
BIN_BASE="/root/FUSEE_CXL/$BUILD/tests/protocol_a_ycsb"
TRACE_DIR="/root/FUSEE_CXL/setup/iter14A_microbench_traces"
DEV=/dev/dax0.0
NB=1048576       # 2^20 = 1M buckets to fit 2M keys at <50% LF
MAX_OPS=2000000  # max trans / load ops
TIMEOUT_S=600
REPS=3

SCENARIOS=(local_read xhost_read local_write xhost_write)
KEYDISTS=(uniform zipf)
T_VALS=(1 4 32 64)
CACHE_VALS=(0 1)  # cold/warm rep cycle; cache=on/off is Protocol A no-op

run_cell() {
  local sc="$1" kd="$2" T="$3" cache_state="$4"  # cache_state: cold|warm
  local cache_flag=1
  # Each scenario × keydist × host has its own trace files
  for rep in $(seq 1 $REPS); do
    local cookie=$(date +%s%N)
    ssh -n g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh -n g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.2
    local load_h0="$TRACE_DIR/bench_${sc}_${kd}_h0.spec_load"
    local trans_h0="$TRACE_DIR/bench_${sc}_${kd}_h0.spec_trans"
    local load_h1="$TRACE_DIR/bench_${sc}_${kd}_h1.spec_load"
    local trans_h1="$TRACE_DIR/bench_${sc}_${kd}_h1.spec_trans"
    cmd_h0="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=bench_${sc}_${kd} timeout $TIMEOUT_S $BIN_BASE $DEV $load_h0 $trans_h0 $NB $MAX_OPS"
    cmd_h1="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=bench_${sc}_${kd} timeout $TIMEOUT_S $BIN_BASE $DEV $load_h1 $trans_h1 $NB $MAX_OPS"
    tag="${sc}_${kd}_T${T}_${cache_state}_rep${rep}"
    ssh -n g3 "$cmd_h0" > "$OUTDIR/${tag}_h0.out" 2>"$OUTDIR/${tag}_h0.err" &
    ssh -n g4 "$cmd_h1" > "$OUTDIR/${tag}_h1.out" 2>"$OUTDIR/${tag}_h1.err" &
    wait
    line=$(grep "^YCSB" "$OUTDIR/${tag}_h0.out" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      wp50=$(echo "$line" | grep -oE 'w_p50_ns=[0-9]+' | cut -d= -f2)
      wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+' | cut -d= -f2)
      rp50=$(echo "$line" | grep -oE 'r_p50_ns=[0-9]+' | cut -d= -f2)
      rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+' | cut -d= -f2)
      echo -e "${sc}\t${kd}\t${T}\t${cache_state}\t${rep}\t${thpt}\t${wp50}\t${wp99}\t${rp50}\t${rp99}" >> "$SUMMARY"
      echo "[OK] ${tag} -> $thpt"
    else
      echo -e "${sc}\t${kd}\t${T}\t${cache_state}\t${rep}\tFAIL\t\t\t\t" >> "$SUMMARY"
      echo "[FAIL] ${tag}"
      tail -3 "$OUTDIR/${tag}_h0.err" "$OUTDIR/${tag}_h1.err" 2>/dev/null | head -10
    fi
  done
}

# Cache state dimension: read scenarios get cold+warm, writes just cold
for sc in "${SCENARIOS[@]}"; do
  is_read=0
  case "$sc" in
    *_read) is_read=1 ;;
  esac
  cache_states=(cold)
  [ $is_read -eq 1 ] && cache_states=(cold warm)
  for kd in "${KEYDISTS[@]}"; do
    for T in "${T_VALS[@]}"; do
      for cstate in "${cache_states[@]}"; do
        run_cell $sc $kd $T $cstate
      done
    done
  done
done

echo "## done. $SUMMARY"
