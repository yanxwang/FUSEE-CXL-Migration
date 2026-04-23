#!/usr/bin/env bash
# Full scaling sweep on g3+g4 (role-mode YCSB):
#   protocols: A, B, C
#   workloads: a, b, c, d, f (e skipped — scan not implemented)
#   threads/host: 1, 2, 4, 8, 16, 32, 64, 86
#   cache modes: configurable via CACHE_MODES env (default "on off")
#
# Output: logs/g34_scaling_sweep_<stamp>/<workload>_<opt>_t<T>_cache<c>/
# Summary file: SUMMARY.log holds one YCSB line per OK run + FAIL markers.
#
# Env tunables:
#   OPTS="A B C"
#   WORKLOADS="workloada workloadb workloadc workloadd workloadf"
#   THREADS="1 2 4 8 16 32 64 86"
#   CACHE_MODES="on off"         # ordered; runs cache=on batch first
#   NUM_BUCKETS=65536
#   MAX_OPS=200000
#   TIMEOUT_S=600

set -u
: "${OPTS:=A B C}"
: "${WORKLOADS:=workloada workloadb workloadc workloadd workloadf}"
: "${THREADS:=1 2 4 8 16 32 64 86}"
: "${CACHE_MODES:=on off}"
: "${NUM_BUCKETS:=65536}"
: "${MAX_OPS:=200000}"
: "${TIMEOUT_S:=600}"
: "${HOST0:=g3}"
: "${HOST1:=g4}"
: "${DEV:=/dev/dax0.0}"

stamp=$(date +%Y%m%d_%H%M%S)
OUT="${OUT_ROOT:-$HOME/FUSEE/logs/g34_scaling_sweep_$stamp}"
mkdir -p "$OUT"
agg="$OUT/SUMMARY.log"
: > "$agg"

{
  echo "# g3+g4 scaling sweep: threads x workloads x opts x cache"
  echo "# host0=$HOST0 host1=$HOST1 dev=$DEV"
  echo "# threads=$THREADS workloads=$WORKLOADS opts=$OPTS cache=$CACHE_MODES"
  echo "# num_buckets=$NUM_BUCKETS max_ops=$MAX_OPS timeout=$TIMEOUT_S"
  echo "# started=$(date -Is)"
} | tee -a "$agg"

fail=0; ok=0
for cache in $CACHE_MODES; do
  for wl in $WORKLOADS; do
    for T in $THREADS; do
      for opt in $OPTS; do
        # A crushes under its ACK-wait at high thread counts. Skip A at
        # T >= A_SKIP_AT (default 64) to match the P1P4P5 baseline's scope
        # and avoid 10-minute timeouts that provide no useful data. Set
        # A_SKIP_AT=999 to re-enable A at every T.
        : "${A_SKIP_AT:=64}"
        if [ "$opt" = "A" ] && [ "$T" -ge "$A_SKIP_AT" ]; then
          continue
        fi
        tag="${wl}_opt${opt}_t${T}_cache${cache}"
        run_dir="$OUT/$tag"
        mkdir -p "$run_dir"
        cookie=$(date +%s%N)
        cenv=""; [ "$cache" = on ] && cenv="FUSEE_CACHE=1 "
        # Forward extra env to the remote runner. FUSEE_A_GROUPS lets the
        # caller enable Phase-5 hierarchical A for high-thread runs.
        [ -n "${FUSEE_A_GROUPS:-}" ] && cenv+="FUSEE_A_GROUPS=$FUSEE_A_GROUPS "
        [ -n "${FUSEE_B_BATCH_K:-}" ] && cenv+="FUSEE_B_BATCH_K=$FUSEE_B_BATCH_K "
        [ -n "${FUSEE_B_BATCH_TIMEOUT_US:-}" ] && cenv+="FUSEE_B_BATCH_TIMEOUT_US=$FUSEE_B_BATCH_TIMEOUT_US "
        [ -n "${FUSEE_SAME_HOST_BYPASS:-}" ] && cenv+="FUSEE_SAME_HOST_BYPASS=$FUSEE_SAME_HOST_BYPASS "
        bin="~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_${opt}"
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
        line=$(grep -m1 "^YCSB " "$run_dir/g3.log" || true)
        if [ -n "$line" ]; then
          echo "$line  # $tag" | tee -a "$agg"
          ok=$((ok + 1))
        else
          echo "# FAIL $tag (see $run_dir/)" | tee -a "$agg"
          fail=$((fail + 1))
          # ssh timeout leaves the remote runner running — it holds the CXL
          # region and blocks subsequent tests. Kill on both hosts before
          # moving on. This pkill targets only the runner binary, not any
          # other processes.
          ssh "$HOST0" "pkill -9 -f 'cxl_ycsb_runner_' 2>/dev/null" &
          ssh "$HOST1" "pkill -9 -f 'cxl_ycsb_runner_' 2>/dev/null" &
          wait
          sleep 1
        fi
      done
    done
  done
done

{
  echo "# finished=$(date -Is)"
  echo "# ok=$ok fail=$fail"
} | tee -a "$agg"
