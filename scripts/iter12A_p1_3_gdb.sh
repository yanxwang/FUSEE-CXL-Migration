#!/usr/bin/env bash
# iter-12A Phase 1.3 — gdb -batch all-worker-process snapshot during
# (hopefully) collapsed rep. Workers are FORKED (separate pids), so we
# enumerate all pids of `protocol_a_ycsb` and attach gdb to each.
#
# Strategy: launch cell, sleep 2.5s into trans phase (collapse takes 8.5s
# wall so we'll catch it mid-spin; win takes 0.04s wall so process exits
# before sleep ends — gdb just won't find pids → records SKIP).
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

SUMMARY="$OUTDIR/${TAG}_gdb_summary.tsv"
echo -e "rep\tclass\twall_s\tthpt_mops\tworker_pids\tdistinct_top_funcs" > "$SUMMARY"

for rep in $(seq 1 $REPS); do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; pkill -9 gdb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.3

  H0_LOG="$OUTDIR/${TAG}_rep${rep}_h0.log"
  H1_LOG="$OUTDIR/${TAG}_rep${rep}_h1.log"

  EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep"
  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
  P1=$!

  # Wait 3s to ensure load_ops_from_file (parses 386 MB workload trace,
  # takes ~1.5-2s) is done and trans has started. Sample at 3s, 5s, 7s.
  # Win reps: workers finish trans by ~T+2s so 3s sample catches aggregation
  # barrier (still gives stacks). Collapse reps: trans runs 2s..10s, so all
  # samples land mid-trans.
  GDB_LOG_PRE="$OUTDIR/${TAG}_rep${rep}_gdb_pre.txt"
  : > "$GDB_LOG_PRE"
  # PIN to absolute time offsets: 3, 5, 7s after rep start
  REP_T0=$(date +%s.%N)
  for sample_at in 3.0 5.0 7.0; do
    # Calculate sleep until sample_at seconds after REP_T0
    now=$(date +%s.%N)
    sleep_dur=$(awk -v t0="$REP_T0" -v at="$sample_at" -v now="$now" 'BEGIN{ d = t0 + at - now; print (d > 0 ? d : 0) }')
    sleep "$sleep_dur"
    SAMPLE_OUT=$(ssh g3 "
      pids=\$(pgrep -x protocol_a_ycsb 2>/dev/null)
      if [ -z \"\$pids\" ]; then
        echo 'GDB_NO_WORKERS_AT_${sample_at}'
        exit 0
      fi
      echo \"GDB_SAMPLING_AT=${sample_at} pids=\$pids\"
      for p in \$pids; do
        echo \"=== PID \$p (at=${sample_at}) ===\"
        cat /proc/\$p/comm 2>/dev/null
        cat /proc/\$p/status 2>/dev/null | grep -E '^(State|Pid|Tgid|PPid|Threads):'
        echo '--- bt:'
        timeout 4 gdb -p \$p -batch \
          -ex 'set pagination off' \
          -ex 'thread apply all bt 20' \
          -ex 'detach' \
          -ex 'quit' 2>&1 | head -100
      done
    " 2>&1)
    echo "$SAMPLE_OUT" >> "$GDB_LOG_PRE"
    echo "--- END SAMPLE at=${sample_at} ---" >> "$GDB_LOG_PRE"
  done

  wait $P0
  wait $P1

  # Parse h0 result
  line=$(grep -E "^YCSB opt=A" "$H0_LOG" | tail -1)
  if [ -z "$line" ]; then
    cls=TIMEOUT
    wall=NA
    thpt_mops=0.000
  else
    wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
    thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
    thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
    cls=$(awk -v m="$thpt_mops" 'BEGIN{ if (m+0 >= 1.0) print "WIN"; else if (m+0 >= 0.3) print "MID"; else print "COLLAPSE" }')
  fi

  GDB_LOG_FINAL="$OUTDIR/${TAG}_${cls}_rep${rep}_gdb.txt"
  mv "$GDB_LOG_PRE" "$GDB_LOG_FINAL" 2>/dev/null

  pids=$(grep -c "^=== PID" "$GDB_LOG_FINAL" 2>/dev/null || echo 0)
  # top frame of each pid: line "#0 ..."
  distinct=$(grep -oE "^#0[ ]+0x[0-9a-f]+ in [^ (]+" "$GDB_LOG_FINAL" 2>/dev/null | sort -u | wc -l || echo 0)

  echo -e "${rep}\t${cls}\t${wall}\t${thpt_mops}\t${pids}\t${distinct}" >> "$SUMMARY"
  echo "[rep $rep] cls=$cls thpt=${thpt_mops} pids_captured=${pids} distinct_top_funcs=${distinct}" >&2
done

echo "[gdb] done. summary: $SUMMARY" >&2
cat "$SUMMARY" >&2
