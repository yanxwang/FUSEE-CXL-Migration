#!/usr/bin/env bash
# iter-15A scaling_ycsb sweep — Protocol A only, post-fix binaries.
#
# Matches docs/scaling_ycsb_spec.md dimensions:
#   workloads = a,b,c,d,f (e skipped, no scan)
#   threads   = 1,2,4,8,16,32,64
#   cache     = on,off
#   kv_size   = 256,512,1024
# = 5 × 7 × 2 × 3 × 1 rep = 210 cells.
#
# Only deviation from spec: MAX_OPS=10M (vs spec 200K). Per user instruction.
#
# Uses tests/protocol_a_ycsb binary (current Protocol A iter-15A build).
# Each kv_size uses its matched build (build-cxl-w1-v{kv}).

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/g34_scaling_ycsb_iter15A_${TS}"
mkdir -p "$OUT_BASE/raw"

WORKLOADS="workloada workloadb workloadc workloadd workloadf"
THREADS="1 2 4 8 16 32 64"
CACHE_MODES="on off"
KV_SIZES="256 512 1024"
MAX_OPS=10000000
NUM_BUCKETS=8388608
TIMEOUT_S=600
DEV=/dev/dax0.0
HOST0=g3
HOST1=g4

SUMMARY="$OUT_BASE/SUMMARY.log"
{
  echo "# iter-15A scaling_ycsb sweep (post-fix binaries)"
  echo "# host0=$HOST0 host1=$HOST1 dev=$DEV"
  echo "# workloads=$WORKLOADS threads=$THREADS cache=$CACHE_MODES kv=$KV_SIZES"
  echo "# num_buckets=$NUM_BUCKETS max_ops=$MAX_OPS timeout=$TIMEOUT_S"
  echo "# started=$(date -Is)"
  echo "# git_sha=$(git -C $ROOT rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$SUMMARY"

# Preflight
for h in $HOST0 $HOST1; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then
    echo "[PREFLIGHT] host=$h has $n stale procs, aborting" >&2
    ssh root@$h "pgrep -af protocol_a_ycsb | grep -v pgrep" >&2
    exit 1
  fi
done

ok=0; fail=0
total=$(echo $WORKLOADS | wc -w)
total=$((total * $(echo $THREADS | wc -w) * $(echo $CACHE_MODES | wc -w) * $(echo $KV_SIZES | wc -w)))
done_count=0

for kv in $KV_SIZES; do
  BUILD="build-cxl-w1-v${kv}"
  for cache in $CACHE_MODES; do
    cache_flag=0; [ "$cache" = on ] && cache_flag=1
    for wl in $WORKLOADS; do
      for T in $THREADS; do
        done_count=$((done_count + 1))
        tag="${wl}_optA_t${T}_cache${cache}_kv${kv}"
        run_dir="$OUT_BASE/raw/$tag"
        mkdir -p "$run_dir"
        cookie=$RANDOM$RANDOM
        t_start=$(date +%s)

        cmd0="cd /root/FUSEE_CXL/${BUILD} && FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$kv FUSEE_REP=1 FUSEE_WORKLOAD_NAME=$wl ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $MAX_OPS"
        cmd1="cd /root/FUSEE_CXL/${BUILD} && FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$kv FUSEE_REP=1 FUSEE_WORKLOAD_NAME=$wl ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/${wl}.spec_load /root/FUSEE_CXL/setup/workloads/${wl}.spec_trans $NUM_BUCKETS $MAX_OPS"

        timeout "$TIMEOUT_S" ssh root@$HOST0 "$cmd0" > "$run_dir/g3.log" 2>&1 &
        p0=$!
        sleep 0.2
        timeout "$TIMEOUT_S" ssh root@$HOST1 "$cmd1" > "$run_dir/g4.log" 2>&1 &
        p1=$!
        wait $p0 $p1 2>/dev/null

        t_end=$(date +%s)
        wallclock=$((t_end - t_start))

        line=$(grep -m1 "^YCSB " "$run_dir/g3.log" || true)
        if [ -n "$line" ]; then
          # Add kv_size and tag to the line for parsing
          echo "$line  kv_size=$kv  # $tag  wallclock=${wallclock}s" | tee -a "$SUMMARY"
          ok=$((ok + 1))
        else
          echo "# FAIL $tag wallclock=${wallclock}s (see raw/$tag/)" | tee -a "$SUMMARY"
          fail=$((fail + 1))
          ssh root@$HOST0 "pkill -9 -f protocol_a_ycsb 2>/dev/null" &
          ssh root@$HOST1 "pkill -9 -f protocol_a_ycsb 2>/dev/null" &
          wait
          sleep 2
        fi
        echo "  [progress] $done_count/$total cells, ok=$ok fail=$fail (last: $tag ${wallclock}s)"
      done
    done
  done
done

{
  echo "# finished=$(date -Is)"
  echo "# ok=$ok fail=$fail"
} | tee -a "$SUMMARY"
echo "==done=="
echo "OUT: $OUT_BASE"
