#!/usr/bin/env bash
# iter-14A P6 v2 Layer 2 — probe-enabled microbench for 4 scenarios × 2 keyDists at T=64.
# Goal: capture stage counts (R0_tls_hit, R2hit, R2miss, R3, etc.) per scenario so we
# can directly answer "does local_read trigger fast path? does xhost_read trigger R3?"
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/PROBE_SUMMARY.tsv"
echo -e "scenario\tkeydist\thost\ttrans_agg_thpt\tw_p50_ns\tw_p99_ns\tr_p50_ns\tr_p99_ns" > "$SUMMARY"

BUILD="${BUILD:-build-cxl-w1-probe}"
BIN_BASE="/root/FUSEE_CXL/$BUILD/tests/protocol_a_ycsb"
TRACE_DIR="/root/FUSEE_CXL/setup/iter14A_microbench_traces"
DEV=/dev/dax0.0
NB=1048576
MAX_OPS=200000  # P4 used 20k; this is 200k for warmup-vs-steady distinguishability
TIMEOUT_S=600
T=64
KV=1024

SCENARIOS=(local_read xhost_read local_write xhost_write)
KEYDISTS=(uniform zipf)

for sc in "${SCENARIOS[@]}"; do
  for kd in "${KEYDISTS[@]}"; do
    local_outdir="$OUTDIR/${sc}_${kd}"
    mkdir -p "$local_outdir"
    cookie=$(date +%s%N)
    ssh -n g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null; rm -rf /root/FUSEE_CXL/setup/probe_dump 2>/dev/null; mkdir -p /root/FUSEE_CXL/setup/probe_dump"
    ssh -n g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null; rm -rf /root/FUSEE_CXL/setup/probe_dump 2>/dev/null; mkdir -p /root/FUSEE_CXL/setup/probe_dump"
    sleep 0.3

    load_h0="$TRACE_DIR/bench_${sc}_${kd}_h0.spec_load"
    trans_h0="$TRACE_DIR/bench_${sc}_${kd}_h0.spec_trans"
    load_h1="$TRACE_DIR/bench_${sc}_${kd}_h1.spec_load"
    trans_h1="$TRACE_DIR/bench_${sc}_${kd}_h1.spec_trans"

    # parse_probes_v4.py rglobs "probe.*.*" — PROBE_DUMP prefix MUST be "probe"
    cmd_h0="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=probe_${sc}_${kd} FUSEE_PROBE_DUMP=/root/FUSEE_CXL/setup/probe_dump/probe timeout $TIMEOUT_S $BIN_BASE $DEV $load_h0 $trans_h0 $NB $MAX_OPS"
    cmd_h1="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=probe_${sc}_${kd} FUSEE_PROBE_DUMP=/root/FUSEE_CXL/setup/probe_dump/probe timeout $TIMEOUT_S $BIN_BASE $DEV $load_h1 $trans_h1 $NB $MAX_OPS"

    ssh -n g3 "$cmd_h0" > "$local_outdir/h0.out" 2>"$local_outdir/h0.err" &
    ssh -n g4 "$cmd_h1" > "$local_outdir/h1.out" 2>"$local_outdir/h1.err" &
    wait

    # Parse on host: aggregate probe dump → per-stage TSV (stdout is the table)
    ssh -n g3 "cd /root/FUSEE_CXL && python3 scripts/parse_probes_v4.py /root/FUSEE_CXL/setup/probe_dump" > "$local_outdir/parsed_h0.tsv" 2>"$local_outdir/parse_h0.log"
    ssh -n g4 "cd /root/FUSEE_CXL && python3 scripts/parse_probes_v4.py /root/FUSEE_CXL/setup/probe_dump" > "$local_outdir/parsed_h1.tsv" 2>"$local_outdir/parse_h1.log"

    for h in h0 h1; do
      line=$(grep "^YCSB" "$local_outdir/$h.out" 2>/dev/null | tail -1)
      if [ -n "$line" ]; then
        thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
        wp50=$(echo "$line" | grep -oE 'w_p50_ns=[0-9]+' | cut -d= -f2)
        wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+' | cut -d= -f2)
        rp50=$(echo "$line" | grep -oE 'r_p50_ns=[0-9]+' | cut -d= -f2)
        rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+' | cut -d= -f2)
        echo -e "${sc}\t${kd}\t${h}\t${thpt}\t${wp50}\t${wp99}\t${rp50}\t${rp99}" >> "$SUMMARY"
      fi
    done
    echo "[$(date +%H:%M:%S)] ${sc} ${kd}: done; YCSB found h0=$(grep -c "^YCSB" "$local_outdir/h0.out") h1=$(grep -c "^YCSB" "$local_outdir/h1.out")"
  done
done

echo "## P6 v2 Layer 2 done. SUMMARY: $SUMMARY"
