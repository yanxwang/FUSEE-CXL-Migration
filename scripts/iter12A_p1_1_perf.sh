#!/usr/bin/env bash
# iter-12A Phase 1.1 (revised) — perf record -g sampling profile of
# protocol_a_ycsb on g3, run for N reps, classify each rep and label
# perf.data + perf.script accordingly.
#
# Key insight: collapse reps spend 99% of their wall-time in the
# spinning/retrying code path. perf samples at 99 Hz × N_cores will
# show those hot functions clearly even with sampling perturbation.
set -u

OUTDIR="${1:?usage: $0 <out_dir> <wl> <T> <cache_on|off> <kv> <reps>}"
WL="${2:?}"
T="${3:?}"
CACHE_STR="${4:?}"
KV="${5:?}"
REPS="${6:?}"
if [ "$CACHE_STR" = "on" ]; then CACHE=1; else CACHE=0; fi
mkdir -p "$OUTDIR"
TAG="${WL}_T${T}_${CACHE_STR}_kv${KV}"

BIN_PATH=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
NUM_BUCKETS=65536
MAX_OPS=50000
TIMEOUT_S=90
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL"

SUMMARY="$OUTDIR/${TAG}_perf_summary.tsv"
echo -e "rep\tclass\twall_s\tthpt_mops\tw_avg_us\tr_avg_us\tperf_samples\ttop_func" > "$SUMMARY"

for rep in $(seq 1 $REPS); do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; pkill -9 perf 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null; rm -f /root/iter12A_perf_${rep}.data' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.3

  H0_LOG="$OUTDIR/${TAG}_rep${rep}_h0.log"
  H1_LOG="$OUTDIR/${TAG}_rep${rep}_h1.log"

  EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep"
  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  # Run protocol_a_ycsb on g3 wrapped by perf record -F 99 -g (sample at 99 Hz
  # to keep overhead low). Use --call-graph fp first (try lbr if fp fails).
  # Wrap in sh -c so the shell built-in `cd` works inside perf record's child.
  PERF_CMD="perf record -F 99 -g --call-graph fp -o /root/iter12A_perf_${rep}.data -- sh -c '$CMD_H0'"
  ssh g3 "$PERF_CMD" > "$H0_LOG" 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
  P1=$!
  wait $P0
  wait $P1

  # Parse h0 result
  line=$(grep -E "^YCSB opt=A" "$H0_LOG" | tail -1)
  if [ -z "$line" ]; then
    cls=TIMEOUT
    wall=NA
    thpt_mops=0.000
    w_us=NA
    r_us=NA
  else
    wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
    thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
    w_avg=$(echo "$line" | grep -oE "w_avg_ns=[0-9]+" | cut -d= -f2)
    r_avg=$(echo "$line" | grep -oE "r_avg_ns=[0-9]+" | cut -d= -f2)
    thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
    w_us=$(awk -v t="$w_avg" 'BEGIN{printf "%.3f", t/1000}')
    r_us=$(awk -v t="$r_avg" 'BEGIN{printf "%.3f", t/1000}')
    cls=$(awk -v m="$thpt_mops" 'BEGIN{ if (m+0 >= 1.0) print "WIN"; else if (m+0 >= 0.3) print "MID"; else print "COLLAPSE" }')
  fi

  # Pull perf report (top 20 functions by overhead)
  ssh g3 "perf report -i /root/iter12A_perf_${rep}.data --stdio --no-children --percent-limit 0.5 2>/dev/null | head -60" > "$OUTDIR/${TAG}_${cls}_rep${rep}_perf_report.txt"
  # Sample count
  samples=$(ssh g3 "perf report -i /root/iter12A_perf_${rep}.data --stdio --header 2>/dev/null | grep '# Samples:' | head -1 | awk '{print \$3}'")
  # Top function
  top_func=$(grep -E "^[ ]+[0-9]+\.[0-9]+%" "$OUTDIR/${TAG}_${cls}_rep${rep}_perf_report.txt" | head -1 | awk '{for(i=NF;i>0;i--){if($i!~/^[0-9.%]/){print $i;exit}}}')

  echo -e "${rep}\t${cls}\t${wall}\t${thpt_mops}\t${w_us}\t${r_us}\t${samples:-NA}\t${top_func:-NA}" >> "$SUMMARY"
  echo "[rep $rep] cls=$cls thpt=${thpt_mops} samples=${samples:-NA} top=${top_func:-NA}" >&2
done

echo "[perf] done. summary: $SUMMARY" >&2
cat "$SUMMARY" >&2
