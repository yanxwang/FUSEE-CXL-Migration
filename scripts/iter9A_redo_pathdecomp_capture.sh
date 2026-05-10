#!/usr/bin/env bash
# iter-9A redo Phase 3 path_decomp capture: workload-A KV=1024 T=64
# cache=on. Per task_plan_iter9A.md §Phase 3.
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
HEALTHY="$OUTDIR/probes_healthy"
ANOMALY="$OUTDIR/probes_anomaly"

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0

run_one() {
  local label=$1
  local local_dir=$2  # local path under $OUTDIR
  local cookie=$(date +%s%N)
  local remote_dir=/tmp/iter9A_redo_pd_${label//./_}
  ssh g3 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV" >/dev/null 2>&1 &
  ssh g4 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV" >/dev/null 2>&1 &
  wait
  echo "=== $label cookie=$cookie ==="
  ssh g3 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloada timeout 120 $BIN $DEV $WL/workloada.spec_load $WL/workloada.spec_trans 65536 200000 2>&1 | grep YCSB" > /tmp/h0_pathdecomp.log &
  ssh g4 "cd /root/FUSEE_CXL/build-cxl && FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=64 FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=1024 FUSEE_WORKLOAD_NAME=workloada timeout 120 $BIN $DEV $WL/workloada.spec_load $WL/workloada.spec_trans 65536 200000 2>&1 > /dev/null" &
  wait
  cat /tmp/h0_pathdecomp.log
  # Pull probe files back to local_dir (only after run, not via run_one).
  echo "$remote_dir" > /tmp/last_remote_dir
}

# Healthy capture: at least 1 try where thpt >= 1 Mops/s (1e6 ops/s)
HEALTHY_OK=0
for try in 1 2 3 4 5; do
  echo "--- Healthy try $try ---"
  run_one "healthy.$try" "$HEALTHY/try${try}"
  thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0_pathdecomp.log | head -1 | cut -d= -f2)
  if [ -n "$thpt" ] && [ "$thpt" -gt 1000000 ]; then
    mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
    echo ">>> HEALTHY: try $try thpt=$thpt = $mops Mops/s <<<"
    # Pull probes back for the healthy run
    REMOTE=$(cat /tmp/last_remote_dir)
    mkdir -p "$HEALTHY/g3_try${try}" "$HEALTHY/g4_try${try}"
    rsync -a g3:${REMOTE}/ "$HEALTHY/g3_try${try}/" 2>&1 | tail -3
    rsync -a g4:${REMOTE}/ "$HEALTHY/g4_try${try}/" 2>&1 | tail -3
    HEALTHY_OK=1
    HEALTHY_THPT=$thpt
    HEALTHY_MOPS=$mops
    HEALTHY_TRY=$try
    break
  fi
done

if [ "$HEALTHY_OK" = "0" ]; then
  echo ">>> HEALTHY capture FAILED after 5 tries (this cell is consistently slow on iter-9A redo arch)"
fi

# Anomaly capture: any try where thpt < HEALTHY/2 (or absolute < 0.1 Mops/s)
# iter-9A original found this cell ~9.89 Mops/s healthy, no anomaly observed.
# Iter-9A redo may or may not have anomalies — try up to 12.
ANOMALY_OK=0
ANOM_THRESHOLD=$((HEALTHY_THPT / 10))
[ "$HEALTHY_OK" = "0" ] && ANOM_THRESHOLD=100000  # 0.1 Mops/s default
for try in 1 2 3 4 5 6 7 8 9 10 11 12; do
  echo "--- Anomaly try $try ---"
  run_one "anomaly.$try" "$ANOMALY/try${try}"
  thpt=$(grep -oE 'trans_agg_thpt=[0-9]+' /tmp/h0_pathdecomp.log | head -1 | cut -d= -f2)
  if [ -z "$thpt" ]; then thpt=0; fi
  if [ "$thpt" -lt "$ANOM_THRESHOLD" ]; then
    mops=$(awk "BEGIN { printf \"%.4f\", $thpt / 1000000 }")
    echo ">>> ANOMALY: try $try thpt=$thpt = $mops Mops/s (<$ANOM_THRESHOLD threshold)"
    REMOTE=$(cat /tmp/last_remote_dir)
    mkdir -p "$ANOMALY/g3_try${try}" "$ANOMALY/g4_try${try}"
    rsync -a g3:${REMOTE}/ "$ANOMALY/g3_try${try}/" 2>&1 | tail -3
    rsync -a g4:${REMOTE}/ "$ANOMALY/g4_try${try}/" 2>&1 | tail -3
    ANOMALY_OK=1
    ANOM_THPT=$thpt
    ANOM_MOPS=$mops
    ANOM_TRY=$try
    break
  fi
done

# Summary
{
  echo "# iter-9A redo path_decomp Phase 1 capture summary"
  echo
  echo "**Cell**: workload-A KV=1024 T=64 cache=on"
  echo "**Captured**: $(date -Is)"
  echo
  echo "## Healthy"
  if [ "$HEALTHY_OK" = "1" ]; then
    echo "- Captured at try=$try, thpt=$HEALTHY_THPT = $HEALTHY_MOPS Mops/s"
    echo "- Probes: $HEALTHY/g3_try${try}/, $HEALTHY/g4_try${try}/"
  else
    echo "- ❌ FAILED after 5 tries"
  fi
  echo
  echo "## Anomaly"
  if [ "$ANOMALY_OK" = "1" ]; then
    echo "- Captured at try=$try, thpt=$ANOM_THPT = $ANOM_MOPS Mops/s"
    echo "- Threshold was thpt < $ANOM_THRESHOLD (= healthy/10)"
    echo "- Probes: $ANOMALY/g3_try${try}/, $ANOMALY/g4_try${try}/"
  else
    echo "- ✓ NO ANOMALY observed in 12 tries (cell is reliably healthy on iter-9A redo arch)"
  fi
} > "$OUTDIR/CAPTURE_SUMMARY.md"

cat "$OUTDIR/CAPTURE_SUMMARY.md"
