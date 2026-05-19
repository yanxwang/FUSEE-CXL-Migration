#!/usr/bin/env bash
# iter-14A P3.C — quantify FUSEE_PROBE=1 overhead vs FUSEE_PROBE=0.
# Same canonical cell × 10 reps × 2 builds.
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.tsv"
echo -e "build\trep\ttrans_agg_thpt" > "$SUMMARY"

REPS=10
WL=workloada
T=64
KV=1024
CACHE=on
MAX_OPS=200000
NUM_BUCKETS=65536
TIMEOUT_S=600
DEV=/dev/dax0.0
WL_DIR=/root/FUSEE_CXL/setup/workloads

cache_flag=1
for build in build-cxl-w1 build-cxl-w1-probe; do
  BIN=/root/FUSEE_CXL/$build/tests/protocol_a_ycsb
  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.2
    cmd="FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL timeout $TIMEOUT_S $BIN $DEV $WL_DIR/$WL.spec_load $WL_DIR/$WL.spec_trans $NUM_BUCKETS $MAX_OPS"
    ssh g3 "FUSEE_HOST_ID=0 $cmd" > "$OUTDIR/${build}_rep${rep}_h0.out" 2>"$OUTDIR/${build}_rep${rep}_h0.err" &
    ssh g4 "FUSEE_HOST_ID=1 $cmd" > "$OUTDIR/${build}_rep${rep}_h1.out" 2>"$OUTDIR/${build}_rep${rep}_h1.err" &
    wait
    line=$(grep "^YCSB" "$OUTDIR/${build}_rep${rep}_h0.out" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      echo -e "$build\t$rep\t$thpt" >> "$SUMMARY"
      echo "[OK] $build rep=$rep -> $thpt"
    else
      echo -e "$build\t$rep\tFAIL" >> "$SUMMARY"
      echo "[FAIL] $build rep=$rep"
    fi
  done
done

echo "## done. SUMMARY at $SUMMARY"
python3 -c "
import statistics
d = {}
with open('$SUMMARY') as f:
    next(f)
    for line in f:
        b, r, t = line.strip().split('\t')
        if t == 'FAIL': continue
        d.setdefault(b, []).append(int(t))
for b, ts in d.items():
    print(f'{b}: median={statistics.median(ts):,.0f} ops/s, n={len(ts)}')
if 'build-cxl-w1' in d and 'build-cxl-w1-probe' in d:
    a = statistics.median(d['build-cxl-w1'])
    bb = statistics.median(d['build-cxl-w1-probe'])
    print(f'probe overhead: {(bb-a)/a*100:+.2f}%')
"
