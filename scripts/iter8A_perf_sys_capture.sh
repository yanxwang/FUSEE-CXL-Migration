#!/usr/bin/env bash
# iter-8A Sol-2/3 v3: system-wide perf record + perf sched record (filter PID at parse)
set -u
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0

for try in 1 2 3 4 5 6 7 8 9 10; do
  cookie=$(date +%s%N)
  ssh g3 'rm -rf /tmp/probe8A_full /tmp/perf.data /tmp/perf.sched.data; mkdir -p /tmp/probe8A_full; chmod 666 /dev/dax0.0; pkill -9 perf 2>/dev/null; pkill -9 protocol_a_ycsb 2>/dev/null' >/dev/null 2>&1 &
  ssh g4 'rm -rf /tmp/probe8A_full; mkdir -p /tmp/probe8A_full; chmod 666 /dev/dax0.0; pkill -9 protocol_a_ycsb 2>/dev/null' >/dev/null 2>&1 &
  wait
  sleep 1
  echo "=== try $try ==="

  # Pre-launch system-wide perf BEFORE the binary starts (60s budget on g3)
  ssh g3 'perf record -a -g -F 99 -o /tmp/perf.data -- sleep 60 2>/tmp/perf.err' &
  ssh g3 'perf sched record -a -o /tmp/perf.sched.data -- sleep 60 2>/tmp/perf.sched.err' &
  sleep 1

  ssh g4 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=/tmp/probe8A_full/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloadd timeout 90 $BIN $DEV $WL/workloadd.spec_load $WL/workloadd.spec_trans 65536 200000 > /dev/null 2>&1" &

  ssh g3 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=/tmp/probe8A_full/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloadd timeout 90 $BIN $DEV $WL/workloadd.spec_load $WL/workloadd.spec_trans 65536 200000 2>&1 | grep YCSB" > /tmp/h0.log

  cat /tmp/h0.log
  thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0.log | head -1 | cut -d= -f2)
  wait

  if [ -n "$thpt" ] && [ "$thpt" -lt 5000000 ]; then
    mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
    echo ">>> COLLAPSED on try $try (thpt=$thpt = $mops Mops/s) <<<"
    ssh g3 'ls -la /tmp/perf.data /tmp/perf.sched.data /tmp/perf.err /tmp/perf.sched.err 2>&1'
    exit 0
  fi
done
echo ">>> 10 tries no collapse <<<"
exit 1
