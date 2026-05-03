#!/usr/bin/env bash
# iter-7A Phase 1: 25 anomalous cells × 5 reps reproducibility check.
set -u
OUT="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUT"
SUMMARY="$OUT/SUMMARY.log"
: > "$SUMMARY"
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads
BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
NUM_BUCKETS=65536
MAX_OPS=200000
TIMEOUT_S=90
REPS=1

# Format: wl kv T cache
CELLS=(
  "a 256 16 on" "a 1024 16 on" "a 512 64 off"
  "b 256 4 on"  "b 1024 64 on" "b 256 64 off"
  "c 256 16 on" "c 512 64 on"  "c 1024 4 on"  "c 1024 16 on" "c 1024 64 off"
  "d 256 32 on" "d 256 64 on"  "d 512 32 on"  "d 1024 4 on"
  "d 1024 32 on" "d 1024 64 on" "d 256 8 off"  "d 256 64 off" "d 1024 4 off" "d 1024 32 off" "d 1024 64 off"
  "f 256 8 on"  "f 512 16 on"  "f 512 32 on"
)
total=$((${#CELLS[@]} * REPS))
echo "[phase1] $total runs across ${#CELLS[@]} cells"
done_runs=0
fail_runs=0
start=$(date +%s)

for cell in "${CELLS[@]}"; do
  read wlc kv T cache_mode <<< "$cell"
  cache=0; [ "$cache_mode" = "on" ] && cache=1
  wl="workload$wlc"
  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
    wait
    sleep 0.2
    cmd_h0="cd /root/FUSEE_CXL/build-cxl && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NUM_BUCKETS $MAX_OPS"
    cmd_h1="${cmd_h0/HOST_ID=0/HOST_ID=1}"
    ssh g3 "$cmd_h0" > /tmp/p1_h0.$$ 2>/dev/null &
    h0=$!
    ssh g4 "$cmd_h1" > /tmp/p1_h1.$$ 2>/dev/null &
    h1=$!
    wait $h0; rc0=$?
    wait $h1; rc1=$?
    if [ $rc0 -eq 0 ] && [ -s /tmp/p1_h0.$$ ]; then
      sed "s|# \(.*\)$|# \1_kv${kv}|" /tmp/p1_h0.$$ >> "$SUMMARY"
    else
      echo "FAIL opt=A wl=$wl T=$T cache=$cache_mode kv=$kv rep=$rep rc0=$rc0 rc1=$rc1" >> "$SUMMARY"
      fail_runs=$((fail_runs + 1))
    fi
    rm -f /tmp/p1_h0.$$ /tmp/p1_h1.$$
    done_runs=$((done_runs + 1))
    elapsed=$(($(date +%s) - start))
    if [ $((done_runs % 10)) -eq 0 ]; then
      echo "[phase1] $done_runs/$total done ($fail_runs fails); elapsed ${elapsed}s"
    fi
  done
done
echo "[phase1] DONE $done_runs runs, $fail_runs fails, $(($(date +%s) - start))s"
