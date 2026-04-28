#!/usr/bin/env bash
# iter-3A Phase 1 cross-host hash-diff battery.
#
# 5 reps × T ∈ {2, 4, 8} × workload A 100K UPDATE = 15 runs.
# Each run: both hosts dump local hash table state to /tmp/h<H>_<rep>_T<T>.bin,
# orchestrator scps + cmp's the two dumps. Diff is on the bucket-array
# section only (bytes 16 .. 16 + num_buckets * 112). cache_epoch_arr is
# host-local; intentionally excluded.
#
# Usage:
#   bash scripts/iter3A_hash_diff_battery.sh [FUSEE_PER_SLOT_LFM_A_value] [FUSEE_K_CHANNELS_value]
# Default 0/1; pass to test per-slot LFM (P2) or K-channel (P4).
set -u

PER_SLOT="${1:-0}"
K_CHAN="${2:-1}"
DEV=/dev/dax0.0
BUCKET_BYTES=$((65536 * 112))   # 65536 buckets × 7 slots × 16B = 7.34 MB
DIFF_OFFSET=16                  # skip 8B magic + 4B host_id + 4B num_buckets

echo "## Hash-diff battery (PER_SLOT_LFM_A=$PER_SLOT, K_CHAN=$K_CHAN) ##"
fail_count=0
pass_count=0

for rep in 1 2 3 4 5; do
  for T in 2 4 8; do
    cookie=$(date +%s%N)
    H0_DUMP=/tmp/iter3A_h0_r${rep}_T${T}_PS${PER_SLOT}_K${K_CHAN}.bin
    H1_DUMP=/tmp/iter3A_h1_r${rep}_T${T}_PS${PER_SLOT}_K${K_CHAN}.bin
    L0_DUMP=$(basename $H0_DUMP)
    L1_DUMP=$(basename $H1_DUMP)
    H0_CMD="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_PER_SLOT_LFM_A=$PER_SLOT FUSEE_K_CHANNELS=$K_CHAN FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_FINAL_STATE_DUMP=$H0_DUMP /root/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV /root/FUSEE_CXL/setup_workloads/workloada.spec_load /root/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 100000"
    H1_CMD="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_PER_SLOT_LFM_A=$PER_SLOT FUSEE_K_CHANNELS=$K_CHAN FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_FINAL_STATE_DUMP=$H1_DUMP /root/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV /root/FUSEE_CXL/setup_workloads/workloada.spec_load /root/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 100000"

    timeout 90 ssh g3 "$H0_CMD" > /tmp/iter3A_g3_r${rep}_T${T}.log 2>&1 &
    sleep 0.3
    timeout 90 ssh g4 "$H1_CMD" > /tmp/iter3A_g4_r${rep}_T${T}.log 2>&1 &
    wait

    # Pull dumps back via scp (ignore stderr).
    scp g3:$H0_DUMP /tmp/$L0_DUMP 2>/dev/null
    scp g4:$H1_DUMP /tmp/$L1_DUMP 2>/dev/null
    if [ ! -f /tmp/$L0_DUMP ] || [ ! -f /tmp/$L1_DUMP ]; then
      echo "rep=$rep T=$T : DUMP-MISSING"
      fail_count=$((fail_count+1))
      continue
    fi
    # Compare bucket section (skip first 16 bytes; compare next BUCKET_BYTES).
    if cmp -s -i $DIFF_OFFSET:$DIFF_OFFSET -n $BUCKET_BYTES /tmp/$L0_DUMP /tmp/$L1_DUMP; then
      echo "rep=$rep T=$T : PASS (cmp = 0)"
      pass_count=$((pass_count+1))
    else
      bytes_diff=$(cmp -l -i $DIFF_OFFSET:$DIFF_OFFSET -n $BUCKET_BYTES /tmp/$L0_DUMP /tmp/$L1_DUMP 2>/dev/null | wc -l)
      echo "rep=$rep T=$T : FAIL ($bytes_diff bytes diff)"
      fail_count=$((fail_count+1))
    fi
    # Cleanup remote dumps to avoid disk fill.
    ssh g3 "rm -f $H0_DUMP" 2>/dev/null &
    ssh g4 "rm -f $H1_DUMP" 2>/dev/null &
    wait
  done
done

echo
echo "## Result: $pass_count PASS / $fail_count FAIL of 15 ##"
[ "$fail_count" -eq 0 ] && exit 0 || exit 1
