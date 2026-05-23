#!/usr/bin/env bash
# Exp 2: perf lock-contention sampling on receivers, T=32 N=4 worker_id,
# zipf-0.99 vs uniform xhost_write. Single rep per dist (diagnostic).
#
# Lock primitive in receiver hot path = pthread_spin_lock (cxl_directory.h).
# Spin-locks waste *cycles*, not syscalls — so perf-cs is useless; instead
# we sample (a) perf record -g on receiver tids → top symbol breakdown,
# (b) perf stat for cycles/instructions/cache → IPC + LLC contention.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter17A_exp2_perf_lock_$TS
mkdir -p $OUT
echo "OUT=$OUT"

NUM_BUCKETS=8388608
TRANS_OPS=20000000    # trace has 20M lines; binary will cap here
DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024
CB=131072
TRACE=/tmp/microbench_traces
T=32; N=4; ROUTING=worker_id
# Load: ~6s (2M load_ops @ ~350k/s). Trans: ~4s zipf, ~3s uniform.
PERF_SECONDS=3
SETTLE_SECONDS=7       # wait for load to finish + first trans ops

run_cell() {
  local dist=$1     # zipf or uniform
  local trace_h0 trace_h1
  if [ "$dist" = zipf ]; then
    trace_h0=$TRACE/bench_xhost_write_zipf-0.99_h0
    trace_h1=$TRACE/bench_xhost_write_zipf-0.99_h1
  else
    trace_h0=$TRACE/bench_xhost_write_uniform_h0
    trace_h1=$TRACE/bench_xhost_write_uniform_h1
  fi
  local cookie=$RANDOM$RANDOM
  echo "[exp2] dist=$dist cookie=$cookie"

  for h in g3 g4; do
    ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 2

  # Launch h0 (g3) + h1 (g4) in background. Long timeout so perf has time.
  timeout 90 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$ROUTING \
    ./tests/protocol_a_ycsb $DEV \
      ${trace_h0}.spec_load ${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/${dist}_h0.out 2>&1 &
  local g3_pid=$!
  timeout 90 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=xhost_write FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=$N FUSEE_RING_ROUTING=$ROUTING \
    ./tests/protocol_a_ycsb $DEV \
      ${trace_h1}.spec_load ${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/${dist}_h1.out 2>&1 &
  local g4_pid=$!

  # Wait for benchmark steady state (load finished, trans started).
  sleep $SETTLE_SECONDS

  # Find write/read/inval receiver tids on g3
  local recv_tids
  recv_tids=$(ssh root@g3 "
    YPID=\$(pgrep -x protocol_a_ycsb | head -1)
    [ -z \"\$YPID\" ] && exit 1
    echo YPID=\$YPID >&2
    for t in \$(ls /proc/\$YPID/task); do
      c=\$(cat /proc/\$YPID/task/\$t/comm 2>/dev/null)
      case \"\$c\" in
        WriteRecv*|ReadRecv*|InvalRecv*) echo -n \"\$t \" ;;
      esac
    done
    echo
  " 2>$OUT/${dist}_tid_lookup.err | tr -s ' ' | sed 's/ *$//;s/ /,/g')
  echo "[exp2] $dist recv_tids=$recv_tids"
  echo "$recv_tids" > $OUT/${dist}_recv_tids.txt
  if [ -z "$recv_tids" ]; then
    echo "[exp2] $dist NO TIDS FOUND, abort"
    wait $g3_pid $g4_pid 2>/dev/null
    return 1
  fi

  # perf stat: IPC + LLC contention
  ssh root@g3 "perf stat -e cycles,instructions,cache-misses,cache-references,LLC-loads,LLC-load-misses,branch-misses --tid $recv_tids sleep $PERF_SECONDS 2>&1" > $OUT/${dist}_perf_stat.txt &
  local pstat_pid=$!

  # perf record with DWARF-unwound callgraph (binary has debug_info)
  ssh root@g3 "perf record -F 99 --call-graph dwarf,8192 --tid $recv_tids -o /tmp/exp2_${dist}.data sleep $PERF_SECONDS 2>&1" > $OUT/${dist}_perf_record_log.txt &
  local prec_pid=$!

  wait $pstat_pid $prec_pid
  echo "[exp2] $dist perf done, dumping report"

  ssh root@g3 "perf report -i /tmp/exp2_${dist}.data --stdio --no-children --percent-limit 1 2>/dev/null" > $OUT/${dist}_perf_report.txt
  ssh root@g3 "perf report -i /tmp/exp2_${dist}.data --stdio --children --percent-limit 1 2>/dev/null" > $OUT/${dist}_perf_report_children.txt

  # Wait for benchmark to finish
  wait $g3_pid $g4_pid 2>/dev/null

  # Extract thpt sanity
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/${dist}_h0.out | head -1)
  echo "[exp2] $dist thpt=${thpt:-0} (cluster ops/s)"
  echo "thpt=$thpt" > $OUT/${dist}_sanity.txt
}

run_cell zipf
run_cell uniform

echo
echo "=== Summary ==="
for dist in zipf uniform; do
  echo "--- $dist ---"
  cat $OUT/${dist}_sanity.txt
  echo "--- $dist top symbols (no-children) ---"
  head -40 $OUT/${dist}_perf_report.txt
done

echo "OUT: $OUT"
