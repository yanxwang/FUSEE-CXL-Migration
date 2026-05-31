#!/usr/bin/env bash
# iter-19A Phase 1.1: PMU attribution for Anomaly A (cache_pct sweep).
#
# Wraps protocol_a_ycsb in `perf stat -e <events>` and parses the
# counter output to attribute throughput drop at large cache_pct to:
#   - LLC-load-miss rate (A-H1: working set > L3)
#   - dTLB-load-miss rate (A-H2: pages > dTLB capacity)
#
# Grid: cache_pct ∈ {1,2,5,10,20,50,100} × 3 reps × zipf-0.99 local_read

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase1_1_pmu_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

declare -A CACHE_BUCKETS=(
  [1]=16384  [2]=32768  [5]=65536  [10]=131072
  [20]=262144  [50]=524288  [100]=2097152
)

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
BUILD=build-cxl-w1-v1024
TRACE=/tmp/microbench_traces
V=1024
T=64

EVENTS=cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,dTLB-loads,dTLB-load-misses

echo "id,V,T,cache_pct,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,cycles,instructions,IPC,L1dload,L1dmiss,L1dmiss_pct,LLCload,LLCmiss,LLCmiss_pct,dTLBload,dTLBmiss,dTLBmiss_pct,wallclock_s" > $CSV

extract_metric() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}

# Extract perf stat counter; perf prints to stderr with thousand separators.
extract_perf() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -E "^\s*[\d,.]+ +$key( |\$|:)" "$file" 2>/dev/null | head -1 | awk '{print $1}' | tr -d ',')
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}

run_cell() {
  local cache_pct=$1 rep=$2
  local cb=${CACHE_BUCKETS[$cache_pct]}
  local id="ph1_1_c${cache_pct}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_zipf-0.99_h0
  local trace_h1=bench_local_read_zipf-0.99_h1

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)

  # Note: perf stat -- inherits all child processes; the binary forks
  # T-1 workers post-spawn so child counters are captured.
  timeout 240 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    perf stat -e $EVENTS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 240 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    perf stat -e $EVENTS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h1}.spec_load $TRACE/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  pid1=$!
  wait $pid0
  wait $pid1
  local t1=$(date +%s); local wc=$((t1-t0))

  local thpt=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')

  # Sum perf counters across h0 + h1
  sum_counter() {
    local key=$1
    local v0=$(extract_perf $out_h0 $key 0)
    local v1=$(extract_perf $out_h1 $key 0)
    awk -v a=$v0 -v b=$v1 'BEGIN{print a+b}'
  }
  local cycles=$(sum_counter cycles)
  local instructions=$(sum_counter instructions)
  local l1dload=$(sum_counter L1-dcache-loads)
  local l1dmiss=$(sum_counter L1-dcache-load-misses)
  local llcload=$(sum_counter LLC-loads)
  local llcmiss=$(sum_counter LLC-load-misses)
  local dtlbload=$(sum_counter dTLB-loads)
  local dtlbmiss=$(sum_counter dTLB-load-misses)

  local ipc=$(awk -v c=$cycles -v i=$instructions 'BEGIN{if(c>0) printf "%.3f", i/c; else print 0}')
  local l1dmiss_pct=$(awk -v m=$l1dmiss -v l=$l1dload 'BEGIN{if(l>0) printf "%.2f", m*100.0/l; else print 0}')
  local llcmiss_pct=$(awk -v m=$llcmiss -v l=$llcload 'BEGIN{if(l>0) printf "%.2f", m*100.0/l; else print 0}')
  local dtlbmiss_pct=$(awk -v m=$dtlbmiss -v l=$dtlbload 'BEGIN{if(l>0) printf "%.2f", m*100.0/l; else print 0}')

  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$id,$V,$T,$cache_pct,$cb,local_read,zipf-0.99,$rep,$thpt_mops,$r_p50_us,$cycles,$instructions,$ipc,$l1dload,$l1dmiss,$l1dmiss_pct,$llcload,$llcmiss,$llcmiss_pct,$dtlbload,$dtlbmiss,$dtlbmiss_pct,$wc" >> $CSV
  echo "  c=$cache_pct rep=$rep thpt=$thpt_mops Mops IPC=$ipc LLCmiss=$llcmiss_pct% dTLBmiss=$dtlbmiss_pct% wc=${wc}s"
}

echo "==== Phase 1.1 — PMU attribution sweep (cache_pct × zipf-0.99) ===="
for cache in 1 2 5 10 20 50 100; do
  for rep in 1 2 3; do
    run_cell $cache $rep
  done
done

echo ""
echo "==== Phase 1.1 verdict ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
print()
print(f"| cache% | thpt (Mops) |   IPC  | LLC miss% | dTLB miss% |")
print(f"|-------:|------------:|-------:|----------:|-----------:|")
for c in ["1","2","5","10","20","50","100"]:
    sel = [r for r in rows if r["cache_pct"]==c]
    if not sel: continue
    thpt = statistics.median([float(r["thpt_Mops"]) for r in sel])
    ipc = statistics.median([float(r["IPC"]) for r in sel])
    llc = statistics.median([float(r["LLCmiss_pct"]) for r in sel])
    dtlb = statistics.median([float(r["dTLBmiss_pct"]) for r in sel])
    print(f"|   {c:>4} | {thpt:10.2f} | {ipc:6.3f} | {llc:8.2f}% | {dtlb:9.2f}% |")
print()
# Correlation: high LLC + dTLB miss rate at large cache_pct → confirms hypothesis
c1 = [r for r in rows if r["cache_pct"]=="1"]
c100 = [r for r in rows if r["cache_pct"]=="100"]
if c1 and c100:
    llc_1 = statistics.median([float(r["LLCmiss_pct"]) for r in c1])
    llc_100 = statistics.median([float(r["LLCmiss_pct"]) for r in c100])
    dtlb_1 = statistics.median([float(r["dTLBmiss_pct"]) for r in c1])
    dtlb_100 = statistics.median([float(r["dTLBmiss_pct"]) for r in c100])
    print(f"=== Verdict ===")
    print(f"LLC miss%  delta c1→c100: {llc_1:.1f}% → {llc_100:.1f}%  (Δ {llc_100-llc_1:+.1f} pp)")
    print(f"dTLB miss% delta c1→c100: {dtlb_1:.1f}% → {dtlb_100:.1f}%  (Δ {dtlb_100-dtlb_1:+.1f} pp)")
    print()
    if llc_100 - llc_1 > 20:
        print(f"→ A-H1 (LLC capacity) CONFIRMED — LLC miss rate scales with cache_pct")
    elif llc_100 - llc_1 > 5:
        print(f"→ A-H1 PARTIAL evidence")
    if dtlb_100 - dtlb_1 > 20:
        print(f"→ A-H2 (dTLB capacity) CONFIRMED — dTLB miss rate scales with cache_pct")
    elif dtlb_100 - dtlb_1 > 5:
        print(f"→ A-H2 PARTIAL evidence")
PY
echo "OUT: $OUT"
