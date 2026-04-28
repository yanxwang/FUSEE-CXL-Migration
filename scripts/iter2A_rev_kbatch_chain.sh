#!/usr/bin/env bash
# Phase 6.5 — K batching sweep. Launch AFTER Phase 6 main sweep
# completes. Sweeps K ∈ {1,2,4,8,16,32,64} × T ∈ {64, T_peak} ×
# workload A cache=on. T_peak is determined from the Phase 6 SUMMARY.
set -u

T_PEAK="${T_PEAK:-4}"
DEV=/dev/dax0.0
OUT_BASE="$HOME/FUSEE/logs/g34_iter2A_rev_kbatch_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_BASE"
agg="$OUT_BASE/SUMMARY.log"
: > "$agg"
echo "# K-batching sweep, T_peak=$T_PEAK, started=$(date -Is)" >> "$agg"

for T in 64 "$T_PEAK"; do
  for K in 1 2 4 8 16 32 64; do
    if [ "$K" -gt "$T" ] && [ "$T" -lt 64 ]; then continue; fi
    cookie=$(date +%s%N)
    tag="K${K}_T${T}_cacheon"
    run_dir="$OUT_BASE/$tag"
    mkdir -p "$run_dir"
    cmd0="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_SENDER_BATCH_K=$K FUSEE_SENDER_BATCH_T_US=20 FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV ~/FUSEE_CXL/setup_workloads/workloada.spec_load ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000"
    cmd1="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_SENDER_BATCH_K=$K FUSEE_SENDER_BATCH_T_US=20 FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV ~/FUSEE_CXL/setup_workloads/workloada.spec_load ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000"
    echo "--- $tag cookie=$cookie ---" | tee -a "$agg"
    timeout 300 ssh g3 "$cmd0" > "$run_dir/g3.log" 2>&1 &
    sleep 0.3
    timeout 300 ssh g4 "$cmd1" > "$run_dir/g4.log" 2>&1 &
    wait
    line=$(grep -m1 '^YCSB ' "$run_dir/g3.log")
    if [ -n "$line" ]; then
      echo "$line  # $tag" | tee -a "$agg"
    else
      echo "# FAIL $tag" | tee -a "$agg"
      ssh g3 'pkill -9 -f cxl_ycsb_runner_A' 2>&1 &
      ssh g4 'pkill -9 -f cxl_ycsb_runner_A' 2>&1 &
      wait
    fi
  done
done

# Timeout-path validation: K=128, T_us=50, T=64.
cookie=$(date +%s%N)
tag="K128_T64_Tus50_cacheon"
run_dir="$OUT_BASE/$tag"
mkdir -p "$run_dir"
cmd0="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_SENDER_BATCH_K=128 FUSEE_SENDER_BATCH_T_US=50 FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=64 ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV ~/FUSEE_CXL/setup_workloads/workloada.spec_load ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000"
cmd1="FUSEE_CACHE=1 FUSEE_PER_HOST_RING=1 FUSEE_SENDER_BATCH_K=128 FUSEE_SENDER_BATCH_T_US=50 FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=64 ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_A $DEV ~/FUSEE_CXL/setup_workloads/workloada.spec_load ~/FUSEE_CXL/setup_workloads/workloada.spec_trans 65536 200000"
echo "--- $tag cookie=$cookie ---" | tee -a "$agg"
timeout 300 ssh g3 "$cmd0" > "$run_dir/g3.log" 2>&1 &
sleep 0.3
timeout 300 ssh g4 "$cmd1" > "$run_dir/g4.log" 2>&1 &
wait
line=$(grep -m1 '^YCSB ' "$run_dir/g3.log")
[ -n "$line" ] && echo "$line  # $tag" | tee -a "$agg" || echo "# FAIL $tag" | tee -a "$agg"
echo "# finished=$(date -Is)" | tee -a "$agg"
echo "OUT=$OUT_BASE"
