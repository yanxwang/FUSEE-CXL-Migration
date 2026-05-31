#!/usr/bin/env bash
# iter-19A Phase 2 fix verification: 6 builds × 4 dist × 3 reps + on-host
# stage decomp at rep=1.
#
# Builds:
#   baseline       = build-cxl-w1-v1024-lrprobe (probe-on baseline; no fix)
#   A_no_lru       = +FUSEE_LR_DEL_LRU_TOUCH (B-H2 mechanism isolation)
#   B_no_flush     = +FUSEE_LR_DEL_OWNER_FLUSH (B-H3 mechanism isolation)
#   C_sample       = +FUSEE_LRU_SAMPLE (1/64 sampling alone)
#   D_pad          = +FUSEE_LRU_PAD (cacheline padding alone)
#   E_sample_pad   = +FUSEE_LRU_SAMPLE +FUSEE_LRU_PAD (combined fix)

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase2_fix_sweep_$TS
mkdir -p $OUT/raw $OUT/decomp
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
TRACE=/tmp/microbench_traces
V=1024
T=64
CB=131072

BUILDS="baseline A_no_lru B_no_flush C_sample D_pad E_sample_pad"

# Push analyzer
scp $ROOT/scripts/iter19A_local_read_decomp_analyze.py g1:/tmp/lr_analyze.py >/dev/null 2>&1

echo "build,V,T,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,r2hit,r2miss_local,lrs2r,lrs4r,cp_lru_evict,wallclock_s" > $CSV

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

map_build() {
  case $1 in
    baseline) echo "build-cxl-w1-v1024-lrprobe" ;;
    *)        echo "build-cxl-w1-v1024-$1" ;;
  esac
}

run_cell() {
  local build_label=$1 keydist=$2 rep=$3
  local builddir=$(map_build $build_label)
  local id="${build_label}_${keydist}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_${keydist}_h0
  local trace_h1=bench_local_read_${keydist}_h1
  local probe_g1=/tmp/lr_probes_${build_label}_${keydist}_rep${rep}
  local probe_g2=/tmp/lr_probes_${build_label}_${keydist}_rep${rep}

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9; rm -f $probe_g1.*" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)
  timeout 180 ssh $H0 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_PROBE_DUMP=$probe_g1 \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 180 ssh $H1 "cd /root/FUSEE_CXL/$builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CB \
    FUSEE_PROBE_DUMP=$probe_g2 \
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
  local r2hit=$(sum_path_agg $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path_agg $out_h0 $out_h1 'r2miss_local')
  local lrs2r=$(sum_path_agg $out_h0 $out_h1 'lrs2r')
  local lrs4r=$(sum_path_agg $out_h0 $out_h1 'lrs4r')
  local cp_lru_evict=$(sum_path_agg $out_h0 $out_h1 'cp_lru_evict')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$build_label,$V,$T,$CB,local_read,$keydist,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r2hit,$r2miss,$lrs2r,$lrs4r,$cp_lru_evict,$wc" >> $CSV
  echo "  $build_label/$keydist rep=$rep thpt=$thpt_mops Mops r_p50=${r_p50_us}us wc=${wc}s"

  if [ "$rep" = "1" ]; then
    local dout=$OUT/decomp/${build_label}_${keydist}.txt
    ssh $H0 "python3 /tmp/lr_analyze.py $probe_g1 --cpu-ghz 2.4 2>&1" > $dout 2>&1
    ssh $H0 "rm -f $probe_g1.*" 2>&1 >/dev/null
    ssh $H1 "rm -f $probe_g2.*" 2>&1 >/dev/null
  fi
}

for build in $BUILDS; do
  echo "==== Build $build ===="
  for dist in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
    for rep in 1 2 3; do
      run_cell $build $dist $rep
    done
  done
done

echo ""
echo "==== SUMMARY ===="
python3 <<PY
import csv, statistics, os
rows = list(csv.DictReader(open("$CSV")))
builds = ["baseline","A_no_lru","B_no_flush","C_sample","D_pad","E_sample_pad"]
dists = ["uniform","zipf-0.5","zipf-0.99","zipf-1.5"]

print()
print("=== Throughput median (Mops, cluster) ===")
print(f"{'build':<15}" + "".join(f"{d:>12}" for d in dists))
for b in builds:
    row = f"{b:<15}"
    for d in dists:
        sel = [float(r["thpt_Mops"]) for r in rows if r["build"]==b and r["keydist"]==d]
        if sel:
            row += f"{statistics.median(sel):>12.2f}"
        else:
            row += f"{'--':>12}"
    print(row)

print()
print("=== zipf-1.5 thpt vs baseline ===")
base_z15 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="baseline" and r["keydist"]=="zipf-1.5"])
base_z099 = statistics.median([float(r["thpt_Mops"]) for r in rows if r["build"]=="baseline" and r["keydist"]=="zipf-0.99"])
print(f"baseline zipf-0.99 = {base_z099:.2f}, baseline zipf-1.5 = {base_z15:.2f}")
print(f"baseline collapse ratio z15/z099 = {base_z15/base_z099:.3f}")
print()
for b in builds[1:]:
    sel_z15 = [float(r["thpt_Mops"]) for r in rows if r["build"]==b and r["keydist"]=="zipf-1.5"]
    sel_z099 = [float(r["thpt_Mops"]) for r in rows if r["build"]==b and r["keydist"]=="zipf-0.99"]
    if not sel_z15 or not sel_z099: continue
    m_z15 = statistics.median(sel_z15)
    m_z099 = statistics.median(sel_z099)
    gain = (m_z15 - base_z15) / base_z15 * 100
    ratio = m_z15 / m_z099
    print(f"  {b:<15} z15={m_z15:6.2f}  (vs baseline {gain:+5.1f}%)  z15/z099 ratio={ratio:.3f}")

print()
print("=== LRS2 / LRS3 per-stage p50 at zipf-1.5 (decomp/<build>_zipf-1.5.txt) ===")
for b in builds:
    fp = f"$OUT/decomp/{b}_zipf-1.5.txt"
    if not os.path.exists(fp): continue
    text = open(fp).read()
    print(f"--- {b} ---")
    # extract HIT path LRS2, MISS path LRS3
    in_hit = False; in_miss = False
    for line in text.splitlines():
        if "HIT path" in line: in_hit=True; in_miss=False; continue
        if "MISS path" in line: in_miss=True; in_hit=False; continue
        line = line.rstrip()
        if in_hit and "LRS2:" in line:
            print(f"  HIT  {line.strip()}")
        elif in_miss and ("LRS3:" in line or "LRS4:" in line):
            print(f"  MISS {line.strip()}")
        elif "StageW" in line and (in_hit or in_miss):
            tag = "HIT " if in_hit else "MISS"
            print(f"  {tag} {line.strip()}")

PY
echo ""
echo "OUT: $OUT"
