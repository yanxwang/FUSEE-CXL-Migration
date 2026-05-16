#!/usr/bin/env bash
# iter-12A Phase 1.1 — run N reps with bpftrace attached on g3.
# bpftrace script lives at /root/iter12A_p1_1_probe.bt on g3 (scp'd by caller).
# For each rep, classify result and label the bpftrace log accordingly so
# WIN vs COLLAPSE traces can be diffed.
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

SUMMARY="$OUTDIR/${TAG}_bpftrace_summary.tsv"
echo -e "rep\tclass\twall_s\tthpt_mops\tw_avg_us\tr_avg_us\tinval_recv\twrite_recv\tread_recv\tinval_sends" > "$SUMMARY"

for rep in $(seq 1 $REPS); do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; pkill -9 bpftrace 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.3

  H0_LOG="$OUTDIR/${TAG}_rep${rep}_h0.log"
  H1_LOG="$OUTDIR/${TAG}_rep${rep}_h1.log"
  BPF_LOG="$OUTDIR/${TAG}_rep${rep}_bpftrace.log"

  EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep"
  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  # Start bpftrace FIRST so the uprobe is in place before the binary loads
  ssh g3 "timeout 35 bpftrace /root/iter12A_p1_1_probe.bt" > "$BPF_LOG" 2>&1 &
  BPF_PID=$!
  sleep 1.5  # give bpftrace time to attach probes

  ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
  P1=$!
  wait $P0
  wait $P1
  # tell bpftrace to exit so we flush counters
  ssh g3 'pkill -INT bpftrace 2>/dev/null' || true
  wait $BPF_PID 2>/dev/null

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
    cls=$(awk -v m="$thpt_mops" 'BEGIN{ if (m+0 >= 1.0) print "WIN"; else if (m+0 >= 0.5) print "MID"; else print "COLLAPSE" }')
  fi

  # Rename bpftrace log with class label for easy diff
  cls_log="$OUTDIR/${TAG}_${cls}_rep${rep}_bpftrace.log"
  mv "$BPF_LOG" "$cls_log" 2>/dev/null

  # Parse bpftrace counters: last 'HEARTBEAT' block before END
  # Each metric prints "@name: COUNT"
  inval_recv=$(grep "^@inval_recv_iters:" "$cls_log" | tail -1 | awk '{print $2}')
  write_recv=$(grep "^@write_recv_iters:" "$cls_log" | tail -1 | awk '{print $2}')
  read_recv=$(grep "^@read_recv_iters:"  "$cls_log" | tail -1 | awk '{print $2}')
  inval_sends=$(grep "^@inval_send_calls:" "$cls_log" | tail -1 | awk '{print $2}')

  echo -e "${rep}\t${cls}\t${wall}\t${thpt_mops}\t${w_us}\t${r_us}\t${inval_recv:-NA}\t${write_recv:-NA}\t${read_recv:-NA}\t${inval_sends:-NA}" >> "$SUMMARY"
  echo "[rep $rep] cls=$cls thpt=${thpt_mops} w=${w_us} r=${r_us} inval_recv=${inval_recv:-NA} write_recv=${write_recv:-NA} read_recv=${read_recv:-NA} inval_sends=${inval_sends:-NA}" >&2
done

echo "[bpftrace] done. summary: $SUMMARY" >&2
cat "$SUMMARY" >&2
