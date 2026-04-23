#!/usr/bin/env bash
# Orchestrator for C-write-path latency decomposition on g3+g4 role-mode.
# For each T in THREADS, runs cxl_latency_decomp_C on both hosts
# simultaneously with a unique RUN_COOKIE, greps the single DECOMP_C line
# from host 0, and appends it to SUMMARY.log.
#
# Env tunables:
#   WORKLOADS="workloada"              # default to A (Zipfian 50/50)
#   THREADS="8 16 32 64"               # plan calls for {8,16,32,64}
#   CACHE_MODES="on"                   # decomp is per-bucket-lock; cache only
#                                      # affects read path which we ignore
#   NUM_BUCKETS=65536
#   MAX_OPS=200000
#   TIMEOUT_S=600
#   HOST0=g3  HOST1=g4  DEV=/dev/dax0.0
#   TAG_SUFFIX=""                      # e.g. "_ticketlock" or "_perslot"
#
# Output: logs/latency_decomp_C_<stamp><suffix>/SUMMARY.log

set -u
: "${WORKLOADS:=workloada}"
: "${THREADS:=8 16 32 64}"
: "${CACHE_MODES:=on}"
: "${NUM_BUCKETS:=65536}"
: "${MAX_OPS:=200000}"
: "${TIMEOUT_S:=600}"
: "${HOST0:=g3}"
: "${HOST1:=g4}"
: "${DEV:=/dev/dax0.0}"
: "${TAG_SUFFIX:=}"

stamp=$(date +%Y%m%d_%H%M%S)
OUT="${OUT_ROOT:-$HOME/FUSEE/logs/latency_decomp_C_${stamp}${TAG_SUFFIX}}"
mkdir -p "$OUT"
agg="$OUT/SUMMARY.log"
: > "$agg"

{
  echo "# C-write-path latency decomposition on $HOST0+$HOST1"
  echo "# dev=$DEV threads=$THREADS workloads=$WORKLOADS cache=$CACHE_MODES"
  echo "# num_buckets=$NUM_BUCKETS max_ops=$MAX_OPS timeout=$TIMEOUT_S"
  echo "# started=$(date -Is)"
} | tee -a "$agg"

ok=0; fail=0
for cache in $CACHE_MODES; do
  for wl in $WORKLOADS; do
    for T in $THREADS; do
      tag="${wl}_t${T}_cache${cache}"
      run_dir="$OUT/$tag"; mkdir -p "$run_dir"
      cookie=$(date +%s%N)
      cenv=""; [ "$cache" = on ] && cenv="FUSEE_CACHE=1 "
      bin="~/FUSEE_CXL/build-cxl/tests/cxl_latency_decomp_C"
      load="~/FUSEE_CXL/setup_workloads/${wl}.spec_load"
      trans="~/FUSEE_CXL/setup_workloads/${wl}.spec_trans"
      cmd0="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
      cmd1="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
      echo "--- $tag cookie=$cookie ---" >> "$agg"
      timeout "$TIMEOUT_S" ssh "$HOST0" "$cmd0" > "$run_dir/g3.log" 2>&1 &
      p0=$!
      sleep 0.3
      timeout "$TIMEOUT_S" ssh "$HOST1" "$cmd1" > "$run_dir/g4.log" 2>&1 &
      p1=$!
      wait $p0 $p1 2>/dev/null
      line=$(grep -m1 '^DECOMP_C ' "$run_dir/g3.log" || true)
      if [ -n "$line" ]; then
        echo "$line  # $tag" | tee -a "$agg"
        ok=$((ok + 1))
      else
        echo "# FAIL $tag (see $run_dir/)" | tee -a "$agg"
        fail=$((fail + 1))
        ssh "$HOST0" "pkill -9 -f 'cxl_latency_decomp_C' 2>/dev/null" &
        ssh "$HOST1" "pkill -9 -f 'cxl_latency_decomp_C' 2>/dev/null" &
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
