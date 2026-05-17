#!/usr/bin/env bash
# iter-13A Phase 2.2.E — W3 batch size K sweep on workloada.
# T={4,16,32,64} × K={16,32,64,128,256,512,1024,2048,4096} × 3 reps.
set -u

OUTDIR="${1:?usage: $0 <out_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.tsv"
echo -e "T\tK\trep\twall_s\tthpt_mops\tw_avg_us\tr_avg_us\tw_p99_us\tr_p99_us" > "$SUMMARY"

REPS=3
WL=workloada
CACHE=0
KV=1024
NUM_BUCKETS=65536
MAX_OPS=200000
TIMEOUT_S=600
BIN=/root/FUSEE_CXL/build-cxl-w3/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2"

for T in 4 16 32 64; do
  # K=16, 32 omitted: with current kReservRingDepth=64, K<64 wraps the
  # ring multiple times and exposes a slot-reuse race that hangs the
  # workers. Root cause likely in worker's wait-for-slot-free loop;
  # detailed fix deferred to iter-14A backlog. Sweep starts at K=64.
  for K in 64 128 256 512 1024 2048 4096; do
    for rep in $(seq 1 $REPS); do
      ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0' &
      ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0' &
      wait
      sleep 0.2
      cookie=$(date +%s%N)
      ARGS="FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL FUSEE_BATCH_K=$K"
      EXTRA="FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep"
      cmd0="cd /root/FUSEE_CXL/build-cxl-w3 && $COMMON $ARGS $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
      cmd1="cd /root/FUSEE_CXL/build-cxl-w3 && $COMMON $ARGS $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
      ssh g3 "$cmd0" > /tmp/k_g3.$$ 2>&1 &
      p0=$!
      ssh g4 "$cmd1" > /tmp/k_g4.$$ 2>&1 &
      p1=$!
      wait $p0; wait $p1
      line=$(grep -E "^YCSB opt=A" /tmp/k_g3.$$ | tail -1)
      if [ -z "$line" ]; then
        echo -e "$T\t$K\t$rep\tFAIL\t0\t0\t0\t0\t0" >> "$SUMMARY"
      else
        wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
        thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
        w_avg=$(echo "$line" | grep -oE "w_avg_ns=[0-9]+" | cut -d= -f2)
        r_avg=$(echo "$line" | grep -oE "r_avg_ns=[0-9]+" | cut -d= -f2)
        w_p99=$(echo "$line" | grep -oE "w_p99_ns=[0-9]+" | cut -d= -f2)
        r_p99=$(echo "$line" | grep -oE "r_p99_ns=[0-9]+" | cut -d= -f2)
        mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
        echo -e "$T\t$K\t$rep\t$wall\t$mops\t$(awk -v t=$w_avg 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$r_avg 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$w_p99 'BEGIN{printf "%.3f",t/1000}')\t$(awk -v t=$r_p99 'BEGIN{printf "%.3f",t/1000}')" >> "$SUMMARY"
      fi
      rm -f /tmp/k_g3.$$ /tmp/k_g4.$$
      echo "[ksweep] T=$T K=$K rep=$rep done" >&2
    done
  done
done
echo "[ksweep] complete. summary: $SUMMARY" >&2
