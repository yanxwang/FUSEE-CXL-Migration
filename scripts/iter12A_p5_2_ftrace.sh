#!/usr/bin/env bash
# iter-12A Phase 5.2 — ftrace sched_switch + sched_wakeup + IRQ events
# on the parent worker thread (pid=tid of pmgr) during a single rep of a
# residual bimodal cell.
#
# Purpose: directly observe whether parent worker (client_id=0, on CPU 0)
# gets involuntarily preempted for > 1 ms during trans phase. The V1
# hypothesis predicts yes (CPU 0 IRQ jitter); if not, V1 is falsified.
#
# Strategy: start trace-cmd record in background BEFORE launching the
# rep, capture all sched_switch + sched_wakeup + irq:* events, run rep,
# stop trace, ship report back, post-process to find > 1 ms preempt gaps.
#
# Usage:
#   scripts/iter12A_p5_2_ftrace.sh <out_dir> <wl> <T> <cache_on|off> <kv>
set -u

OUTDIR="${1:?usage: $0 <out_dir> <wl> <T> <cache_on|off> <kv>}"
WL="${2:?}"
T="${3:?}"
CACHE_STR="${4:?}"
KV="${5:?}"
if [ "$CACHE_STR" = "on" ]; then CACHE=1; else CACHE=0; fi
mkdir -p "$OUTDIR"
TAG="${WL}_T${T}_${CACHE_STR}_kv${KV}"

BIN_PATH=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb   # NON-instrumented build for ftrace (less interference)
WL_DIR=/root/FUSEE_CXL/setup/workloads
NUM_BUCKETS=65536
MAX_OPS=50000
TIMEOUT_S=120
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL"

# Clean state
ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; pkill -9 trace-cmd 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
wait
sleep 0.3

COOKIE=$(date +%s%N)
H0_LOG="$OUTDIR/${TAG}_h0.log"
H1_LOG="$OUTDIR/${TAG}_h1.log"

EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=1"
CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

# Start trace-cmd in background — capture sched switch, wakeup, IRQ events on g3
echo "[5.2] starting trace-cmd on g3..." >&2
ssh g3 "trace-cmd record \
  -e sched:sched_switch \
  -e sched:sched_wakeup \
  -e irq:irq_handler_entry \
  -e irq:irq_handler_exit \
  -e irq:softirq_entry \
  -e irq:softirq_exit \
  -e timer:timer_expire_entry \
  -o /tmp/iter12A_p5_2.dat \
  -- sleep 25 &
  echo \"trace-cmd PID=\$!\" > /tmp/p5_2_trace_pid
" &
T_PID=$!
sleep 1.0

echo "[5.2] starting ycsb rep cookie=$COOKIE..." >&2
ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
P0=$!
ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
P1=$!
wait $P0
wait $P1
wait $T_PID 2>/dev/null

# Parse class
line=$(grep -E "^YCSB opt=A" "$H0_LOG" | tail -1)
if [ -z "$line" ]; then
  cls=TIMEOUT
  wall=NA; thpt_mops=0.000
else
  wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
  thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
  thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
  cls=$(awk -v m="$thpt_mops" 'BEGIN{ if (m+0 >= 1.0) print "WIN"; else if (m+0 >= 0.3) print "MID"; else print "COLLAPSE" }')
fi
echo "[5.2] cls=$cls thpt=${thpt_mops}" >&2

# Generate report on g3 + scp back
ssh g3 "trace-cmd report -i /tmp/iter12A_p5_2.dat 2>/dev/null > /tmp/iter12A_p5_2_${cls}.txt"
mkdir -p "$OUTDIR/${TAG}_${cls}"
scp "g3:/tmp/iter12A_p5_2_${cls}.txt" "$OUTDIR/${TAG}_${cls}/sched_irq.txt" 2>&1 | tail -3

# Cleanup remote
ssh g3 'rm -f /tmp/iter12A_p5_2.dat /tmp/iter12A_p5_2_*.txt /tmp/p5_2_trace_pid'

echo "cookie=$COOKIE cell=$TAG cls=$cls wall=$wall thpt=$thpt_mops" > "$OUTDIR/${TAG}_${cls}/META.txt"

# Quick post-process: count preempt events on cpu 0
RPT="$OUTDIR/${TAG}_${cls}/sched_irq.txt"
if [ -s "$RPT" ]; then
  irq_count=$(grep -c "irq_handler_entry" "$RPT" || echo 0)
  softirq_count=$(grep -c "softirq_entry" "$RPT" || echo 0)
  switch_cpu0=$(awk '/sched_switch:/ && /\[000\]/ {n++} END{print n+0}' "$RPT")
  echo "[5.2] IRQ events: $irq_count, softirq events: $softirq_count, CPU0 sched_switches: $switch_cpu0" | tee -a "$OUTDIR/${TAG}_${cls}/META.txt"
fi
echo "[5.2] done. data at $OUTDIR/${TAG}_${cls}/" >&2
