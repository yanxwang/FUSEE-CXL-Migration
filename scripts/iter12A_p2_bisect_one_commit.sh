#!/usr/bin/env bash
# iter-12A Phase 2 — bisect helper. Check out a specific commit on
# the master copy (~/FUSEE), rsync src/ to g3+g4, rebuild, run 5 reps
# on workload-c T=64 cache=on kv=1024 (the iter-11A Phase 1 reference
# cell), capture trans_agg_thpt + r_avg_ns + w_avg_ns per rep.
#
# Output to $out_dir/$commit.csv: rep,thpt_mops,w_avg_us,r_avg_us
#
# Usage: scripts/iter12A_p2_bisect_one_commit.sh <out_dir> <commit_sha>
set -u

OUTDIR="${1:?usage: $0 <out_dir> <commit_sha>}"
COMMIT="${2:?}"
mkdir -p "$OUTDIR"
RESULT="$OUTDIR/${COMMIT}.csv"
echo "rep,thpt_mops,w_avg_us,r_avg_us,wall_s" > "$RESULT"

cd /home/yanwang/FUSEE

# Remember current state and restore on exit
ORIG_BRANCH=$(git branch --show-current 2>/dev/null)
if [ -z "$ORIG_BRANCH" ]; then
  ORIG_BRANCH=$(git rev-parse HEAD)
fi
trap 'cd /home/yanwang/FUSEE; git checkout -q "'"$ORIG_BRANCH"'" 2>/dev/null; echo "[bisect] restored to $ORIG_BRANCH"' EXIT INT TERM

echo "[bisect $COMMIT] checking out (was on $ORIG_BRANCH)..."
git checkout -q "$COMMIT" || { echo "checkout failed"; exit 1; }

echo "[bisect $COMMIT] rsync src/ + rebuild on g3 + g4..."
rsync -a src/ g3:/root/FUSEE_CXL/src/ &
rsync -a src/ g4:/root/FUSEE_CXL/src/ &
wait
ssh g3 'cd /root/FUSEE_CXL/build-cxl && make -j16 protocol_a_ycsb 2>&1 | tail -3' > /tmp/p2_g3.log 2>&1 &
ssh g4 'cd /root/FUSEE_CXL/build-cxl && make -j16 protocol_a_ycsb 2>&1 | tail -3' > /tmp/p2_g4.log 2>&1 &
wait

if ! grep -q "protocol_a_ycsb" /tmp/p2_g3.log || ! grep -q "protocol_a_ycsb" /tmp/p2_g4.log; then
  echo "[bisect $COMMIT] BUILD FAILED" | tee -a "$RESULT"
  cat /tmp/p2_g3.log | tee -a "$RESULT"
  echo "build failed" > "$OUTDIR/${COMMIT}.FAIL"
  exit 1
fi

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
NUM_BUCKETS=65536
MAX_OPS=50000
TIMEOUT_S=90
WL=workloadc
T=64
CACHE=1
KV=1024
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL"

for rep in 1 2 3 4 5; do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.2

  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  ssh g3 "$CMD_H0" > /tmp/p2_h0.log 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > /tmp/p2_h1.log 2>&1 &
  P1=$!
  wait $P0; wait $P1

  line=$(grep -E "^YCSB opt=A" /tmp/p2_h0.log | tail -1)
  if [ -z "$line" ]; then
    echo "${rep},TIMEOUT,NA,NA,NA" >> "$RESULT"
    echo "[$COMMIT rep $rep] TIMEOUT"
    continue
  fi
  wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
  thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
  w=$(echo "$line" | grep -oE "w_avg_ns=[0-9]+" | cut -d= -f2)
  r=$(echo "$line" | grep -oE "r_avg_ns=[0-9]+" | cut -d= -f2)
  thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
  w_us=$(awk -v t="$w" 'BEGIN{printf "%.3f", t/1000}')
  r_us=$(awk -v t="$r" 'BEGIN{printf "%.3f", t/1000}')
  echo "${rep},${thpt_mops},${w_us},${r_us},${wall}" >> "$RESULT"
  echo "[$COMMIT rep $rep] thpt=${thpt_mops} r_avg=${r_us}us w_avg=${w_us}us wall=${wall}s"
done

# median
echo "[bisect $COMMIT] result:"
cat "$RESULT"
