#!/usr/bin/env bash
# iter-18A Phase 3 per-stage opt smoke test.
# Runs T ∈ {8, 32, 64} × V=1024 × zipf-0.99 × N=0 × FUSEE_CACHE=0, 3 reps
# each. Compares median thpt to a baseline-CSV (from Phase 2.1 probe-off
# T=8/32/64 medians) and prints delta percentage.
#
# Usage:
#   iter18A_phase3_smoke.sh <candidate_tag> <baseline_csv>
#   <candidate_tag> = short label, used as output subdir name
#   <baseline_csv>  = probe-off grid.csv from Phase 2.1
#
# Exit non-zero if any cell times out. User decides keep/revert based on
# printed delta + per-stage decomp (if probe-on dump requested).
set -u
ROOT=/home/yanwang/FUSEE
TAG=${1:?usage: $0 <tag> <baseline_csv>}
BASELINE_CSV=${2:?usage: $0 <tag> <baseline_csv>}
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter18A_phase3_${TAG}_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv
echo "T,rep,thpt,r_p50_us,r_p99_us,wc_s" > $CSV

NUM_BUCKETS=8388608
TRANS_OPS=5000000
DEV=/dev/dax0.0
BUILD=build-cxl-w1-v1024   # probe-off, normal build
CB=131072
TRACE=/tmp/microbench_traces
TBASE=bench_xhost_read_zipf-0.99

Ts=(8 32 64)

kill_all() { for h in g3 g4; do ssh root@$h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1; done; sleep 1; }

run_cell() {
  local T=$1 rep=$2
  local cookie=$RANDOM$RANDOM
  local id="T${T}_rep${rep}"
  kill_all
  local t0=$(date +%s)
  timeout 120 ssh root@g3 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h0.spec_load $TRACE/${TBASE}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h0.out 2>&1 &
  timeout 120 ssh root@g4 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=0 \
    FUSEE_WORKLOAD_NAME=xhost_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_RING_SHARDS_FACTOR=0 FUSEE_RING_ROUTING=worker_id \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${TBASE}_h1.spec_load $TRACE/${TBASE}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $OUT/raw/${id}_h1.out 2>&1 &
  wait
  local t1=$(date +%s); local wc=$((t1-t0))
  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1); [[ -z "$thpt" ]] && thpt=0
  local p50=$(grep -oP 'r_p50_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p99=$(grep -oP 'r_p99_ns=\K[0-9]+' $OUT/raw/${id}_h0.out | head -1)
  local p50_us=$(awk -v n=${p50:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  local p99_us=$(awk -v n=${p99:-0} 'BEGIN{printf "%.2f", n/1000.0}')
  echo "$T,$rep,$thpt,$p50_us,$p99_us,$wc" >> $CSV
  echo "  T=$T rep=$rep thpt=$thpt wc=${wc}s"
}

for T in "${Ts[@]}"; do
  for rep in 1 2 3; do run_cell $T $rep; done
done

echo "=== Decision matrix vs baseline $BASELINE_CSV ==="
python3 - <<PYEOF
import csv, statistics
def medians(path, key_col, t_col, thpt_col, t_filter):
    out = {}
    with open(path) as f:
        rows = list(csv.DictReader(f))
    by_t = {}
    for r in rows:
        T = int(r[t_col])
        if T not in t_filter: continue
        if key_col and r.get(key_col, "off") != "off": continue
        thpt = float(r[thpt_col])
        if thpt > 0:
            by_t.setdefault(T, []).append(thpt)
    return {t: statistics.median(v) for t, v in by_t.items() if v}
base = medians("$BASELINE_CSV", "build", "T", "thpt", set([8, 32, 64]))
cand = medians("$CSV", None, "T", "thpt", set([8, 32, 64]))
print(f"{'T':>4s}  {'baseline_Mops':>14s}  {'candidate_Mops':>15s}  {'delta_pct':>10s}  {'verdict':>10s}")
keep_count = 0; revert_count = 0
for T in (8, 32, 64):
    b = base.get(T); c = cand.get(T)
    if b is None or c is None: print(f"{T:>4d}  {'NA':>14s}  {'NA':>15s}  {'NA':>10s}  {'NA':>10s}"); continue
    delta = (c - b) / b * 100
    if delta >= 5: v = "KEEP"
    elif delta <= -5: v = "REVERT"
    else: v = "AUDIT"
    if v == "KEEP": keep_count += 1
    elif v == "REVERT": revert_count += 1
    print(f"{T:>4d}  {b/1e6:>14.3f}  {c/1e6:>15.3f}  {delta:>+9.1f}%  {v:>10s}")
print()
if revert_count > 0:
    print("→ overall: REVERT (at least one T regressed)")
elif keep_count >= 1:
    print("→ overall: KEEP (≥1 T improved ≥5 %)")
else:
    print("→ overall: AUDIT-ONLY (all within ±5 % noise)")
PYEOF

echo "OUT: $OUT"
