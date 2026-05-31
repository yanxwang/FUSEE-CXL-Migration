#!/usr/bin/env bash
# iter-19A Phase 2.4: local_read 4-dist probe sweep.
#
# For each of (uniform, zipf-0.5, zipf-0.99, zipf-1.5):
#   - 3 reps with build-cxl-w1-v1024-lrprobe (FUSEE_LOCAL_READ_PROBE=1)
#   - Pull probe dumps from g1 + g2 + run iter19A_local_read_decomp_analyze.py
#   - Save per-cell HIT/MISS stage medians
#
# Output:
#   docs/iter19A_phase2_4_lr_decomp_<ts>/
#     grid.csv: per-cell thpt + LRS2R + LRS4R counters
#     decomp_<dist>.txt: stage decomp per dist (HIT + MISS p50)
#     probes_<dist>/: probe binary dumps if needed for re-analysis

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase2_4_lr_decomp_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
BUILD=build-cxl-w1-v1024-lrprobe
TRACE=/tmp/microbench_traces
V=1024
T=64

echo "id,V,T,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,r0_tls,r2hit,r2miss_local,lrs2r,lrs4r,cp_lru_evict,wallclock_s" > $CSV

extract_metric() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}
extract_path_agg() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep "after_TRANS AGG" "$file" 2>/dev/null | head -1 | grep -oP "$key=\K[\d.]+" | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}
sum_path_agg() {
  local f0=$1 f1=$2 key=$3
  local v0=$(extract_path_agg "$f0" "$key" 0)
  local v1=$(extract_path_agg "$f1" "$key" 0)
  awk -v a=$v0 -v b=$v1 'BEGIN{print a+b}'
}

run_cell() {
  local keydist=$1 rep=$2
  local id="ph2_4_${keydist}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_${keydist}_h0
  local trace_h1=bench_local_read_${keydist}_h1
  local cb=131072
  local probe_dir_g1=/tmp/lr_probes_${keydist}_rep${rep}
  local probe_dir_g2=/tmp/lr_probes_${keydist}_rep${rep}

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9; \
            rm -rf $probe_dir_g1.*; mkdir -p $(dirname $probe_dir_g1)" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)
  timeout 180 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    FUSEE_PROBE_DUMP=$probe_dir_g1 \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 180 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    FUSEE_PROBE_DUMP=$probe_dir_g2 \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h1}.spec_load $TRACE/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  pid1=$!
  wait $pid0
  wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract_metric $out_h0 'r_p99_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r0_tls=$(sum_path_agg $out_h0 $out_h1 'r0_tls')
  local r2hit=$(sum_path_agg $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path_agg $out_h0 $out_h1 'r2miss_local')
  local lrs2r=$(sum_path_agg $out_h0 $out_h1 'lrs2r')
  local lrs4r=$(sum_path_agg $out_h0 $out_h1 'lrs4r')
  local cp_lru_evict=$(sum_path_agg $out_h0 $out_h1 'cp_lru_evict')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$id,$V,$T,$cb,local_read,$keydist,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r0_tls,$r2hit,$r2miss,$lrs2r,$lrs4r,$cp_lru_evict,$wc" >> $CSV
  echo "  $id rep=$rep thpt=$thpt_mops Mops lrs2r=$lrs2r lrs4r=$lrs4r wc=${wc}s"

  # Pull probes for rep=1 only (per-rep 8GB+, ship analyses for rep1 cell)
  if [ "$rep" = "1" ]; then
    local probe_local=$OUT/probes_${keydist}_rep1
    mkdir -p $probe_local
    rsync -az g1:$probe_dir_g1.\* $probe_local/ 2>&1 | tail -1
    rsync -az g2:$probe_dir_g2.\* $probe_local/ 2>&1 | tail -1
    echo "  → analyzing $probe_local"
    python3 $ROOT/scripts/iter19A_local_read_decomp_analyze.py $probe_local \
      > $OUT/decomp_${keydist}.txt 2>&1
    # Cleanup remote probes
    ssh $H0 "rm -f $probe_dir_g1.*" 2>&1
    ssh $H1 "rm -f $probe_dir_g2.*" 2>&1
  fi
}

echo "==== Phase 2.4 — local_read 4-dist probe sweep ===="
for dist in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
  for rep in 1 2 3; do
    run_cell $dist $rep
  done
done

echo ""
echo "==== Phase 2.4 summary ===="
python3 <<PY
import csv, statistics, os
rows = list(csv.DictReader(open("$CSV")))
print()
print(f"|  dist       | thpt (Mops) | lrs2r/op   | lrs4r/op   | r2hit rate |")
print(f"|-------------|------------:|-----------:|-----------:|-----------:|")
for d in ["uniform", "zipf-0.5", "zipf-0.99", "zipf-1.5"]:
    sel = [r for r in rows if r["keydist"]==d]
    thpt = statistics.median([float(r["thpt_Mops"]) for r in sel])
    lrs2r = statistics.median([int(r["lrs2r"]) for r in sel])
    lrs4r = statistics.median([int(r["lrs4r"]) for r in sel])
    r2hit = statistics.median([int(r["r2hit"]) for r in sel])
    r2miss = statistics.median([int(r["r2miss_local"]) for r in sel])
    total = r2hit + r2miss
    hr = r2hit / total * 100 if total else 0
    # per-op rate: rough estimate over 5M transactions per host × 2 hosts
    lrs2r_per = lrs2r / 1e7 * 1e6  # per million ops
    lrs4r_per = lrs4r / 1e7 * 1e6
    print(f"| {d:11s} | {thpt:10.2f} | {lrs2r_per:10.2f} | {lrs4r_per:10.2f} | {hr:9.2f}% |")
print()
print(f"per-op rates × 1e6 (i.e., retries per million ops)")
print()

# Verdict
z099 = [r for r in rows if r["keydist"]=="zipf-0.99"]
z15  = [r for r in rows if r["keydist"]=="zipf-1.5"]
if z099 and z15:
    lrs4r_z099 = statistics.median([int(r["lrs4r"]) for r in z099])
    lrs4r_z15  = statistics.median([int(r["lrs4r"]) for r in z15])
    lrs2r_z099 = statistics.median([int(r["lrs2r"]) for r in z099])
    lrs2r_z15  = statistics.median([int(r["lrs2r"]) for r in z15])
    print(f"=== verdict ===")
    if lrs4r_z099 > 0:
        ratio4 = lrs4r_z15 / lrs4r_z099
        print(f"LRS4R (cpool_insert CAS retry) zipf-1.5 / zipf-0.99: {ratio4:.2f}×")
        if ratio4 > 5:
            print(f"  → B-H1 (cpool_insert thundering herd) CONFIRMED")
        elif ratio4 > 2:
            print(f"  → B-H1 PARTIAL evidence")
        else:
            print(f"  → B-H1 REFUTED (no thundering herd inflation)")
    else:
        print(f"LRS4R both = 0 → either no cpool_insert happens (no MISS), or counter broken")
    if lrs2r_z099 > 0:
        ratio2 = lrs2r_z15 / lrs2r_z099
        print(f"LRS2R (seqlock reader CAS retry) zipf-1.5 / zipf-0.99: {ratio2:.2f}×")
        if ratio2 > 5:
            print(f"  → B-H2 (cacheline ping-pong) CONFIRMED")
        elif ratio2 > 2:
            print(f"  → B-H2 PARTIAL evidence")
        else:
            print(f"  → B-H2 REFUTED")
    else:
        print(f"LRS2R both = 0 → no seqlock retries (read-only steady state)")

for d in ["uniform", "zipf-0.5", "zipf-0.99", "zipf-1.5"]:
    fp = os.path.join("$OUT", f"decomp_{d}.txt")
    if os.path.exists(fp):
        print(f"")
        print(f"=== {d} stage decomp (from probes) ===")
        with open(fp) as f:
            for line in f.readlines()[-15:]:
                print(line.rstrip())
PY

echo "OUT: $OUT"
