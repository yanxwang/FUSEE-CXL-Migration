#!/usr/bin/env bash
# iter-12A Phase 0.C / 1.0 / 1.7 — repro a single (wl, T, cache, kv)
# cell N reps and print per-rep aggregate throughput + r_avg / w_avg.
#
# Usage:
#   scripts/iter12A_repro_cell.sh <out_dir> <wl> <T> <cache_on|off> <kv> <reps>
# Example:
#   scripts/iter12A_repro_cell.sh /tmp/p0c_ref workloada 4 off 512 20
#
# Outputs to $out_dir:
#   ${wl}_T${T}_${cache}_kv${kv}_summary.tsv  (rep wall_s thpt_mops w_avg_us r_avg_us)
#   ${wl}_T${T}_${cache}_kv${kv}_repN_h{0,1}.log  raw stdout/stderr
#
# Honored by Phase 0.C (multi-cell scaffolding), Phase 1.0 (reference cell
# repro), Phase 1.7 (gate-12 5-rep verify per bimodal cell).
set -u

OUTDIR="${1:?usage: $0 <out_dir> <wl> <T> <cache_on|off> <kv> <reps>}"
WL="${2:?}"
T="${3:?}"
CACHE_STR="${4:?}"
KV="${5:?}"
REPS="${6:?}"

if [ "$CACHE_STR" = "on" ]; then CACHE=1; else CACHE=0; fi

BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
NUM_BUCKETS=65536
MAX_OPS=50000
TIMEOUT_S=90
COMMON="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_CACHE=$CACHE FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL"

mkdir -p "$OUTDIR"
TAG="${WL}_T${T}_${CACHE_STR}_kv${KV}"
SUMMARY="$OUTDIR/${TAG}_summary.tsv"
echo -e "rep\twall_s\tthpt_mops\tw_avg_us\tr_avg_us" > "$SUMMARY"

echo "[repro] cell=$TAG reps=$REPS out=$OUTDIR" >&2
for rep in $(seq 1 $REPS); do
  COOKIE=$(date +%s%N)
  ssh g3 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  ssh g4 'pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null' &
  wait
  sleep 0.2

  EXTRA_REP="FUSEE_RUN_COOKIE=$COOKIE FUSEE_REP=$rep"
  CMD_H0="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA_REP FUSEE_HOST_ID=0 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"
  CMD_H1="cd /root/FUSEE_CXL/build-cxl && $COMMON $EXTRA_REP FUSEE_HOST_ID=1 timeout $TIMEOUT_S $BIN /dev/dax0.0 $WL_DIR/${WL}.spec_load $WL_DIR/${WL}.spec_trans $NUM_BUCKETS $MAX_OPS"

  H0_LOG="$OUTDIR/${TAG}_rep${rep}_h0.log"
  H1_LOG="$OUTDIR/${TAG}_rep${rep}_h1.log"

  ssh g3 "$CMD_H0" > "$H0_LOG" 2>&1 &
  P0=$!
  ssh g4 "$CMD_H1" > "$H1_LOG" 2>&1 &
  P1=$!
  wait $P0
  wait $P1

  # parse h0 (driver, emits YCSB line)
  line=$(grep -E "^YCSB opt=A" "$H0_LOG" | tail -1)
  if [ -z "$line" ]; then
    # collapsed run may not finish; treat as 0 thpt
    echo -e "${rep}\tTIMEOUT\t0.000000\t0.000\t0.000" >> "$SUMMARY"
    echo "[rep $rep] TIMEOUT (no YCSB line in $H0_LOG)" >&2
    continue
  fi
  wall=$(echo "$line" | grep -oE "trans_wall_max=[0-9.]+" | cut -d= -f2)
  thpt=$(echo "$line" | grep -oE "trans_agg_thpt=[0-9]+" | cut -d= -f2)
  w_avg=$(echo "$line" | grep -oE "w_avg_ns=[0-9]+" | cut -d= -f2)
  r_avg=$(echo "$line" | grep -oE "r_avg_ns=[0-9]+" | cut -d= -f2)
  thpt_mops=$(awk -v t="$thpt" 'BEGIN{printf "%.6f", t/1e6}')
  w_us=$(awk -v t="$w_avg" 'BEGIN{printf "%.3f", t/1000}')
  r_us=$(awk -v t="$r_avg" 'BEGIN{printf "%.3f", t/1000}')
  echo -e "${rep}\t${wall}\t${thpt_mops}\t${w_us}\t${r_us}" >> "$SUMMARY"
  echo "[rep $rep] thpt=${thpt_mops} Mops/s w_avg=${w_us}us r_avg=${r_us}us wall=${wall}s" >&2
done

echo "[repro] done. summary: $SUMMARY" >&2
cat "$SUMMARY" >&2
