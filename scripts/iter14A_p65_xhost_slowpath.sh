#!/usr/bin/env bash
# iter-14A P6.5 — verify local fast vs xhost slow path by forcing cache_pool
# overflow.  NB=1024 instead of NB=1M makes cache_pool overflow heavily so
# R2 misses → R3 fires on xhost reads.
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.tsv"
echo -e "scenario\tkeydist\tT\tNB\trep\ttrans_agg_thpt\tw_p50\tw_p99\tr_p50\tr_p99" > "$SUMMARY"

BUILD="${BUILD:-build-cxl-w1}"
BIN_BASE="/root/FUSEE_CXL/$BUILD/tests/protocol_a_ycsb"
TRACE_DIR="/root/FUSEE_CXL/setup/iter14A_microbench_traces"
DEV=/dev/dax0.0
TIMEOUT_S=600
REPS=3
T=64
KV=1024
MAX_OPS=200000  # reduced from 2M trans — eviction-heavy mode is slow, smaller sample suffices for gap-quantification

SCENARIOS=(local_read xhost_read)
KEYDISTS=(uniform zipf)
NB_VALS=(1024 1048576)  # 1K (overflow) vs 1M (no overflow, control)

run_cell() {
  local sc="$1" kd="$2" NB="$3"
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
    cmd_h0="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=p65_${sc}_${kd}_NB${NB} timeout $TIMEOUT_S $BIN_BASE $DEV $load_h0 $trans_h0 $NB $MAX_OPS"
    cmd_h1="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=p65_${sc}_${kd}_NB${NB} timeout $TIMEOUT_S $BIN_BASE $DEV $load_h1 $trans_h1 $NB $MAX_OPS"
    tag="${sc}_${kd}_NB${NB}_rep${rep}"
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
      echo -e "${sc}\t${kd}\t${T}\t${NB}\t${rep}\t${thpt}\t${wp50}\t${wp99}\t${rp50}\t${rp99}" >> "$SUMMARY"
      echo "[OK] ${tag} -> thpt=$thpt r_p50=$rp50ns r_p99=$rp99ns"
    else
      echo -e "${sc}\t${kd}\t${T}\t${NB}\t${rep}\tFAIL\t\t\t\t" >> "$SUMMARY"
      echo "[FAIL] ${tag}"
      tail -3 "$OUTDIR/${tag}_h0.err" 2>/dev/null | head -10
    fi
  done
}

for sc in "${SCENARIOS[@]}"; do
  for kd in "${KEYDISTS[@]}"; do
    for NB in "${NB_VALS[@]}"; do
      run_cell $sc $kd $NB
    done
  done
done

echo "## done. SUMMARY at $SUMMARY"
