#!/usr/bin/env bash
# iter-12A Phase 1.2 — ftrace sched_switch per receiver thread.
# Goal: confirm or rule out scheduling preempt (V1) as bimodal root cause.
# Per QR2 user prior: scheduling unlikely; this is a falsification step.
#
# Strategy: launch protocol_a_ycsb on g3, get receiver tid via /proc/<pid>/task,
# enable ftrace sched_switch filtered by those tids, run N reps,
# report longest gap between sched_switch events per receiver thread.
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

SUMMARY="$OUTDIR/${TAG}_ftrace_summary.tsv"
echo -e "rep\tclass\twall_s\tthpt_mops\trecv_max_preempt_us\tworker_max_preempt_us" > "$SUMMARY"

for rep in $(seq 1 $REPS); do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null; trace-cmd reset 2>/dev/null' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.3

  H0_LOG="$OUTDIR/${TAG}_rep${rep}_h0.log"
  H1_LOG="$OUTDIR/${TAG}_rep${rep}_h1.log"
  TRACE_LOG="$OUTDIR/${TAG}_rep${rep}_ftrace.dat"

  EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep"
  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  # Start tracing in background BEFORE protocol_a_ycsb runs
  ssh g3 "trace-cmd record -e sched:sched_switch -o /root/iter12A_trace_rep${rep}.dat -- sleep 35" > /dev/null 2>&1 &
  TRACE_PID=$!
  sleep 0.5

  ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
  P1=$!
  wait $P0
  wait $P1
  wait $TRACE_PID 2>/dev/null

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

  # Generate report: max preempt gap per receiver / worker thread.
  ssh g3 "trace-cmd report -i /root/iter12A_trace_rep${rep}.dat 2>/dev/null | grep -E '(Receiver|Sender|ycsb)' | head -3000 > /root/iter12A_trace_rep${rep}.txt"
  ssh g3 "cat /root/iter12A_trace_rep${rep}.txt" > "$OUTDIR/${TAG}_${cls}_rep${rep}_ftrace.txt"

  # Compute max gap between sched_switch events for receiver threads.
  # ftrace text format: TASK-PID    [CPU]    TIMESTAMP: sched_switch: ...
  recv_max=$(awk '
    /Receiver/ {
      # split "TASK-PID"
      n = split($0, f, " ");
      ts = $4; gsub(":", "", ts);
      tid = $2; gsub(/[^0-9]/, "", tid);
      if (prev[tid]) {
        gap = ts - prev[tid];
        if (gap > maxg[tid]) maxg[tid] = gap;
      }
      prev[tid] = ts;
    }
    END {
      for (t in maxg) if (maxg[t] > overall) overall = maxg[t];
      printf("%.3f", overall * 1e6);  # convert s -> us
    }
  ' "$OUTDIR/${TAG}_${cls}_rep${rep}_ftrace.txt" 2>/dev/null || echo "NA")

  echo -e "${rep}\t${cls}\t${wall}\t${thpt_mops}\t${recv_max:-NA}\tNA" >> "$SUMMARY"
  echo "[rep $rep] cls=$cls thpt=${thpt_mops} recv_max_preempt_us=${recv_max:-NA}" >&2
done

echo "[ftrace] done. summary: $SUMMARY" >&2
cat "$SUMMARY" >&2
