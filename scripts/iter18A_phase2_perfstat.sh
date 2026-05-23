#!/usr/bin/env bash
# iter-18A Phase 2.4 — perf stat on ReadRecv tids for T ∈ {1, 8, 64}, V=1024,
# zipf-0.99, N=0. Uses probe-off binary (perf measures hardware counters, not
# probe-related overhead). Need TRANS large enough so steady-state ≥ 4 s.
set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase2_perfstat_$TS
mkdir -p $OUT
NUM_BUCKETS=8388608
TRANS_OPS=20000000   # ~10-20s steady state on read path
DEV=/dev/dax0.0
BUILD_OFF=build-cxl-w1-v1024
CB=131072; TRACE=/tmp/microbench_traces
WORKLOAD=xhost_read; TBASE=bench_xhost_read_zipf-0.99
SETTLE=8       # load takes ~6s, then trans starts
PERF_SECONDS=4

# Need 20M-line traces. Generate if not present.
LOCAL_TRACE=/tmp/iter18a_xhost_read_long_traces
if [ ! -f $LOCAL_TRACE/bench_xhost_read_zipf-0.99_h0.spec_trans ]; then
  mkdir -p $LOCAL_TRACE
  echo "[gen] generating 20M-line xhost_read zipf-0.99 trace (~30 s)..."
  python3 $ROOT/scripts/iter14A_gen_microbench_traces.py $LOCAL_TRACE \
    --num-load 2000000 --num-trans 20000000 \
    --scenarios xhost_read --keydists "zipf-0.99" --num-hosts 2
fi

# Rsync long trace to hosts (replacing 5M one for this sweep)
rsync -avz $LOCAL_TRACE/bench_xhost_read_zipf-0.99_h0.spec_load \
            $LOCAL_TRACE/bench_xhost_read_zipf-0.99_h0.spec_trans \
            root@g3:/tmp/microbench_traces/ > /dev/null 2>&1 &
rsync -avz $LOCAL_TRACE/bench_xhost_read_zipf-0.99_h1.spec_load \
            $LOCAL_TRACE/bench_xhost_read_zipf-0.99_h1.spec_trans \
            root@g4:/tmp/microbench_traces/ > /dev/null 2>&1 &
wait
echo "[gen] long traces in place"

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 2; }

run_cell() {
  local T=$1
  local cookie=$RANDOM$RANDOM
  echo "=== T=$T ==="
  kill_all
  timeout 60 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD_OFF && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h0.spec_load $TRACE/${TBASE}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/T${T}_h0.out 2>&1 &
  local g3p=$!
  timeout 60 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD_OFF && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=$WORKLOAD FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h1.spec_load $TRACE/${TBASE}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/T${T}_h1.out 2>&1 &
  local g4p=$!

  sleep $SETTLE
  # Find ReadRecv tids on g3
  local rtids
  rtids=$(ssh root@g3 "
    YPID=\$(pgrep -x protocol_a_ycsb | head -1)
    [ -z \"\$YPID\" ] && exit 1
    for t in \$(ls /proc/\$YPID/task); do
      c=\$(cat /proc/\$YPID/task/\$t/comm 2>/dev/null)
      case \"\$c\" in
        ReadRecv*) echo -n \"\$t \" ;;
      esac
    done; echo
  " 2>/dev/null | tr -s ' ' | sed 's/ *$//;s/ /,/g')
  echo "  T=$T ReadRecv tids = $rtids"
  echo "$rtids" > $OUT/T${T}_recv_tids.txt
  if [ -z "$rtids" ]; then
    echo "  T=$T NO TIDS (binary may have exited); wait + skip"
    wait $g3p $g4p 2>/dev/null
    return 1
  fi

  ssh root@g3 "perf stat -e cycles,instructions,cache-misses,cache-references,LLC-loads,LLC-load-misses,branch-misses --tid $rtids sleep $PERF_SECONDS 2>&1" > $OUT/T${T}_perf_stat.txt
  wait $g3p $g4p 2>/dev/null
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/T${T}_h0.out | head -1)
  echo "  T=$T thpt=${thpt:-0}, perf stat written"
  echo "thpt=$thpt" > $OUT/T${T}_sanity.txt
}

for T in 1 8 64; do
  run_cell $T
done

echo "=== Per-T perf stat ==="
for T in 1 8 64; do
  echo "--- T=$T ---"
  cat $OUT/T${T}_perf_stat.txt 2>/dev/null
  cat $OUT/T${T}_sanity.txt 2>/dev/null
  echo
done

echo "OUT: $OUT"
