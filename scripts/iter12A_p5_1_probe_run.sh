#!/usr/bin/env bash
# iter-12A Phase 5.1 — run cell with FUSEE_PROBE=1 build, dump per-thread
# probe files, collect locally for post-process.
#
# Uses /root/FUSEE_CXL/build-cxl-p5/tests/protocol_a_ycsb (built with
# -DFUSEE_PROBE=1 macro). Probe dump path:
#   /tmp/iter12A_p5/probe.<pid>.<tid>  (per-thread mmap'd, 128 MB each)
#
# After rep, scp all probe files back to local $out_dir.
#
# Usage:
#   scripts/iter12A_p5_1_probe_run.sh <out_dir> <wl> <T> <cache_on|off> <kv>
set -u

OUTDIR="${1:?usage: $0 <out_dir> <wl> <T> <cache_on|off> <kv>}"
WL="${2:?}"
T="${3:?}"
CACHE_STR="${4:?}"
KV="${5:?}"
if [ "$CACHE_STR" = "on" ]; then CACHE=1; else CACHE=0; fi
mkdir -p "$OUTDIR/g3" "$OUTDIR/g4"
TAG="${WL}_T${T}_${CACHE_STR}_kv${KV}"

BIN_PATH=/root/FUSEE_CXL/build-cxl-p5/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
NUM_BUCKETS=65536
MAX_OPS=50000
TIMEOUT_S=120
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL FUSEE_PROBE_DUMP=/tmp/iter12A_p5/probe"

# Clean any prior probe files
ssh g3 'rm -rf /tmp/iter12A_p5 && mkdir -p /tmp/iter12A_p5' &
ssh g4 'rm -rf /tmp/iter12A_p5 && mkdir -p /tmp/iter12A_p5' &
wait

# Kill any leftover ycsb
ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
wait
sleep 0.3

COOKIE=$(date +%s%N)
H0_LOG="$OUTDIR/${TAG}_h0.log"
H1_LOG="$OUTDIR/${TAG}_h1.log"

EXTRA="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=1"
CMD_H0="cd /root/FUSEE_CXL/build-cxl-p5 && $COMMON $EXTRA FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
CMD_H1="cd /root/FUSEE_CXL/build-cxl-p5 && $COMMON $EXTRA FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN_PATH /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

echo "[5.1] cell=$TAG cookie=$COOKIE" >&2
ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
P0=$!
ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
P1=$!
wait $P0
wait $P1

# Parse classification FIRST (before scp — only scp if collapse)
line=$(grep -E "^YCSB opt=A" "$H0_LOG" | tail -1)
if [ -z "$line" ]; then
  cls=TIMEOUT
  wall=NA; thpt_mops=0.000
else
  wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
  thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
  thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.4f", t/1e6}')
  cls=$(awk -v m="$thpt_mops" 'BEGIN{ if (m+0 >= 1.0) print "WIN"; else if (m+0 >= 0.3) print "MID"; else print "COLLAPSE" }')
fi
echo "[5.1] cls=$cls thpt=${thpt_mops} wall=${wall}s" >&2

mkdir -p "$OUTDIR/${TAG}_${cls}"

# Only scp probe files if this is a COLLAPSE/MID/TIMEOUT (not WIN — waste of bandwidth)
if [ "$cls" = "WIN" ]; then
  echo "[5.1] WIN rep — skipping probe scp (cleanup remote only)" >&2
  ssh g3 'rm -rf /tmp/iter12A_p5' &
  ssh g4 'rm -rf /tmp/iter12A_p5' &
  wait
else
  mkdir -p "$OUTDIR/${TAG}_${cls}/g3" "$OUTDIR/${TAG}_${cls}/g4"
  echo "[5.1] $cls rep — scp probe files g3 + g4 → local in parallel..." >&2
  scp 'g3:/tmp/iter12A_p5/probe.*' "$OUTDIR/${TAG}_${cls}/g3/" > /tmp/scp_g3.log 2>&1 &
  S3=$!
  scp 'g4:/tmp/iter12A_p5/probe.*' "$OUTDIR/${TAG}_${cls}/g4/" > /tmp/scp_g4.log 2>&1 &
  S4=$!
  wait $S3; wait $S4
  cnt_g3=$(ls "$OUTDIR/${TAG}_${cls}/g3/" 2>/dev/null | wc -l)
  cnt_g4=$(ls "$OUTDIR/${TAG}_${cls}/g4/" 2>/dev/null | wc -l)
  echo "[5.1] collected ${cnt_g3} probe files from g3, ${cnt_g4} from g4" >&2
  ssh g3 'rm -rf /tmp/iter12A_p5' &
  ssh g4 'rm -rf /tmp/iter12A_p5' &
  wait
fi

# Record metadata
echo "cookie=${COOKIE} cell=${TAG} class=${cls} wall=${wall} thpt=${thpt_mops}" > "$OUTDIR/${TAG}_${cls}/META.txt"
echo "[5.1] done, dir at $OUTDIR/${TAG}_${cls}/" >&2
