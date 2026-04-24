#!/usr/bin/env bash
# Phase-3.8 OAT batch-size sweep.
#
# For each (K, T_us) cell, run workload A cache=on at T=32 clients, 200 k
# ops, 2 hosts role-mode, and log one DECOMP_C-formatted line (reuses the
# existing cxl_ycsb_runner SUMMARY line — writer thpt + writer p99 come
# from there directly; peer-visibility lag is approximated from w_p99 -
# (T_us + known CXL epoch latency ~3 µs) in post-processing).
#
# Env:
#   KS     - space-separated K values (default plan phase-A set)
#   TS_US  - space-separated T_us values (default plan phase-B set)
#   MERGE  - "on off" (default both) -- selects which MERGE_SAME_KEY binary
#            to invoke.
#   NUM_CLIENTS=32 (plan §3.8)
#
# Binaries: expects
#   ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_C            (MERGE=on)
#   ~/FUSEE_CXL/build-cxl-merge-off/tests/cxl_ycsb_runner_C  (MERGE=off)

set -u
: "${HOST0:=g3}"
: "${HOST1:=g4}"
: "${DEV:=/dev/dax0.0}"
: "${WORKLOAD:=workloada}"
: "${NUM_CLIENTS:=32}"
: "${NUM_BUCKETS:=65536}"
: "${MAX_OPS:=200000}"
: "${TIMEOUT_S:=120}"
: "${KS:=4 8 16 32 64 128 256}"
: "${TS_US:=100}"
: "${MERGE:=on}"

stamp=$(date +%Y%m%d_%H%M%S)
OUT="${OUT_ROOT:-$HOME/FUSEE/logs/batch_size_sweep_${stamp}}"
mkdir -p "$OUT"
agg="$OUT/SUMMARY.log"
: > "$agg"

{
  echo "# Phase-3.8 batch-size sweep"
  echo "# host0=$HOST0 host1=$HOST1 dev=$DEV"
  echo "# workload=$WORKLOAD num_clients=$NUM_CLIENTS num_buckets=$NUM_BUCKETS"
  echo "# max_ops=$MAX_OPS timeout=$TIMEOUT_S"
  echo "# KS=$KS TS_US=$TS_US MERGE=$MERGE"
  echo "# started=$(date -Is)"
} | tee -a "$agg"

ok=0; fail=0
for merge in $MERGE; do
  case "$merge" in
    on)  build="build-cxl" ;;
    off) build="build-cxl-merge-off" ;;
    *)   echo "bad MERGE=$merge"; exit 2 ;;
  esac
  bin="~/FUSEE_CXL/${build}/tests/cxl_ycsb_runner_C"
  load="~/FUSEE_CXL/setup_workloads/${WORKLOAD}.spec_load"
  trans="~/FUSEE_CXL/setup_workloads/${WORKLOAD}.spec_trans"

  for K in $KS; do
    for T in $TS_US; do
      tag="merge${merge}_K${K}_T${T}us"
      run_dir="$OUT/$tag"; mkdir -p "$run_dir"
      cookie=$(date +%s%N)
      cenv="FUSEE_CACHE=1 FUSEE_BATCH_K=$K FUSEE_BATCH_T_US=$T "
      cmd0="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$NUM_CLIENTS $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
      cmd1="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$NUM_CLIENTS $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
      echo "--- $tag cookie=$cookie ---" >> "$agg"
      timeout "$TIMEOUT_S" ssh "$HOST0" "$cmd0" > "$run_dir/g3.log" 2>&1 &
      p0=$!; sleep 0.3
      timeout "$TIMEOUT_S" ssh "$HOST1" "$cmd1" > "$run_dir/g4.log" 2>&1 &
      p1=$!
      wait $p0 $p1 2>/dev/null
      line=$(grep -m1 '^YCSB ' "$run_dir/g3.log" || true)
      if [ -n "$line" ]; then
        echo "$line  # $tag" | tee -a "$agg"
        ok=$((ok+1))
      else
        echo "# FAIL $tag (see $run_dir/)" | tee -a "$agg"
        fail=$((fail+1))
        ssh "$HOST0" "pkill -9 -f 'cxl_ycsb_runner_' 2>/dev/null" &
        ssh "$HOST1" "pkill -9 -f 'cxl_ycsb_runner_' 2>/dev/null" &
        wait
        sleep 1
      fi
    done
  done
done

{
  echo "# finished=$(date -Is)"
  echo "# ok=$ok fail=$fail"
} | tee -a "$agg"
echo "$OUT"
