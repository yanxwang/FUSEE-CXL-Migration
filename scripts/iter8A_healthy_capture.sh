#!/usr/bin/env bash
# Capture probes during a HEALTHY run of d kv=1024 T=64 cache=on.
# Per A.1: 17/20 healthy → 1 try should succeed; budget 5 tries.
set -u
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0

for try in 1 2 3 4 5; do
  cookie=$(date +%s%N)
  ssh g3 'rm -rf /tmp/probe8A_healthy; mkdir -p /tmp/probe8A_healthy; chmod 666 /dev/dax0.0' >/dev/null 2>&1 &
  ssh g4 'rm -rf /tmp/probe8A_healthy; mkdir -p /tmp/probe8A_healthy; chmod 666 /dev/dax0.0' >/dev/null 2>&1 &
  wait
  echo "=== try $try ==="
  ssh g3 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=/tmp/probe8A_healthy/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloadd timeout 90 $BIN $DEV $WL/workloadd.spec_load $WL/workloadd.spec_trans 65536 200000 2>&1 | grep YCSB" > /tmp/h0.log &
  ssh g4 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=/tmp/probe8A_healthy/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloadd timeout 90 $BIN $DEV $WL/workloadd.spec_load $WL/workloadd.spec_trans 65536 200000 2>&1 > /dev/null" &
  wait
  cat /tmp/h0.log
  thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0.log | head -1 | cut -d= -f2)
  if [ -n "$thpt" ] && [ "$thpt" -gt 10000000 ]; then
    mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
    echo ">>> HEALTHY on try $try (thpt=$thpt = $mops Mops/s) <<<"
    exit 0
  fi
done
echo ">>> 5 tries no healthy run <<<"
exit 1
