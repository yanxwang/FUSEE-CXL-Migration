#!/usr/bin/env bash
# iter-13A Phase 2.5 — full scaling_ycsb sweep with W1 build.
# 210 runs (5 wl × 7 T × 2 cache × 3 KV × 1 rep).
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
SUMMARY="$OUTDIR/SUMMARY.log"
mkdir -p "$OUTDIR"
: > "$SUMMARY"

WORKLOADS="${WORKLOADS:-workloada workloadb workloadc workloadd workloadf}"
THREADS="${THREADS:-1 2 4 8 16 32 64}"
CACHE_MODES="${CACHE_MODES:-on off}"
KV_SIZES="${KV_SIZES:-256 512 1024}"
REPS="${REPS:-1}"
NUM_BUCKETS=65536
MAX_OPS=200000
TIMEOUT_S=600
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads
BIN=/root/FUSEE_CXL/build-cxl-w1/tests/protocol_a_ycsb

COMMON_ENV="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0"

total_runs=0
for wl in $WORKLOADS; do
  for cache in $CACHE_MODES; do
    for kv in $KV_SIZES; do
      for t in $THREADS; do
        for rep in $(seq 1 $REPS); do
          total_runs=$((total_runs + 1))
        done
      done
    done
  done
done

{
  echo "# iter-13A Phase 2.5 sweep — W1 build (HAZARD read + RESERVED write)"
  echo "# total_runs=$total_runs reps=$REPS max_ops=$MAX_OPS"
  echo "# started=$(date -Is)"
  echo "# commit=$(git -C /home/yanwang/FUSEE rev-parse HEAD 2>/dev/null || echo unknown)"
} | tee -a "$SUMMARY"

done_runs=0
fail_runs=0
start_ts=$(date +%s)

for wl in $WORKLOADS; do
  for cache_mode in $CACHE_MODES; do
    cache=0; [ "$cache_mode" = "on" ] && cache=1
    for kv in $KV_SIZES; do
      for t in $THREADS; do
        for rep in $(seq 1 $REPS); do
          cookie=$(date +%s%N)
          ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
          ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
          wait
          sleep 0.2
          cmd_h0="cd /root/FUSEE_CXL/build-cxl-w1 && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$t FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NUM_BUCKETS $MAX_OPS"
          cmd_h1="cd /root/FUSEE_CXL/build-cxl-w1 && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$t FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NUM_BUCKETS $MAX_OPS"
          ssh g3 "$cmd_h0" > /tmp/h0_out.$$ 2>/tmp/h0_err.$$ &
          h0pid=$!
          ssh g4 "$cmd_h1" > /tmp/h1_out.$$ 2>/tmp/h1_err.$$ &
          h1pid=$!
          wait $h0pid; rc0=$?
          wait $h1pid; rc1=$?
          if [ $rc0 -eq 0 ] && [ -s /tmp/h0_out.$$ ]; then
            sed "s|# \(.*\)$|# \1_kv${kv}|" /tmp/h0_out.$$ >> "$SUMMARY"
          else
            echo "FAIL opt=A wl=$wl T=$t cache=$cache_mode kv=$kv rep=$rep rc0=$rc0 rc1=$rc1" >> "$SUMMARY"
            fail_runs=$((fail_runs + 1))
          fi
          rm -f /tmp/h0_out.$$ /tmp/h1_out.$$ /tmp/h0_err.$$ /tmp/h1_err.$$
          done_runs=$((done_runs + 1))
          elapsed=$(($(date +%s) - start_ts))
          if [ $((done_runs % 20)) -eq 0 ]; then
            eta_s=$(( elapsed * (total_runs - done_runs) / (done_runs > 0 ? done_runs : 1) ))
            echo "[sweep] $done_runs/$total_runs done ($fail_runs fails); elapsed ${elapsed}s; eta ${eta_s}s"
          fi
        done
      done
    done
  done
done

{
  echo "# finished=$(date -Is)"
  echo "# ok=$((done_runs - fail_runs))  fail=$fail_runs  total=$done_runs"
  echo "# elapsed=$(($(date +%s) - start_ts))s"
} | tee -a "$SUMMARY"
