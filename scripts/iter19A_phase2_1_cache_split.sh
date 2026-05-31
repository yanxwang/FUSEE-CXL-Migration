#!/usr/bin/env bash
# iter-19A Phase 2.1: cache=0 vs cache=on @ zipf-0.99 vs zipf-1.5.
# Critical split test for Anomaly B root cause:
#   - cache=0 (no L2) means search() falls straight to CXL bucket scan
#   - If cache=0 also collapses at zipf-1.5 → root cause is in hashtable
#     bucket path (B-H3 hot bucket cacheline ping-pong)
#   - If cache=0 stays normal at zipf-1.5 → root cause is in cache path
#     (B-H1 cpool_insert thundering / B-H2 cacheline ping-pong / B-H4
#     LRU evict storm)
#
# Grid (local_read scenario only):
#   dist ∈ {zipf-0.99, zipf-1.5} × cache ∈ {0, 1} × 3 reps = 12 cells
#   V=1024 T=64 cache_pct=10 (irrelevant when FUSEE_CACHE=0)

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase2_1_cache_split_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
BUILD=build-cxl-w1-v1024
TRACE=/tmp/microbench_traces
V=1024
T=64

echo "id,V,T,cache_pct,cache_buckets,scenario,keydist,fusee_cache,rep,thpt_Mops,r_p50_us,r_p99_us,r0_tls,r2hit,r2miss_local,cpool_ins_r,cp_lru_evict,wallclock_s" > $CSV

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
  local keydist=$1 fusee_cache=$2 rep=$3
  local id="ph2_1_${keydist}_cache${fusee_cache}"
  local cookie=$RANDOM$RANDOM
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out
  local trace_h0=bench_local_read_${keydist}_h0
  local trace_h1=bench_local_read_${keydist}_h1
  local cb=131072

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1
  local t0=$(date +%s)
  timeout 180 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$fusee_cache \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 180 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$fusee_cache \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
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
  local cpool_ins_r=$(sum_path_agg $out_h0 $out_h1 'cpool_ins_r')
  local cp_lru_evict=$(sum_path_agg $out_h0 $out_h1 'cp_lru_evict')
  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')

  echo "$id,$V,$T,10,$cb,local_read,$keydist,$fusee_cache,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r0_tls,$r2hit,$r2miss,$cpool_ins_r,$cp_lru_evict,$wc" >> $CSV
  echo "  $id rep=$rep cache=$fusee_cache thpt=$thpt_mops Mops r_p50=${r_p50_us}us wc=${wc}s"
}

echo "==== Phase 2.1 — cache split test ===="
for dist in zipf-0.99 zipf-1.5; do
  for fc in 0 1; do
    for rep in 1 2 3; do
      run_cell $dist $fc $rep
    done
  done
done

echo ""
echo "==== Phase 2.1 verdict ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
def med(filters):
    sel = [float(r['thpt_Mops']) for r in rows if all(r[k]==v for k,v in filters.items())]
    return statistics.median(sel) if sel else 0

z099_c1 = med({"keydist":"zipf-0.99", "fusee_cache":"1"})
z099_c0 = med({"keydist":"zipf-0.99", "fusee_cache":"0"})
z15_c1  = med({"keydist":"zipf-1.5",  "fusee_cache":"1"})
z15_c0  = med({"keydist":"zipf-1.5",  "fusee_cache":"0"})

print(f"")
print(f"|             | cache=1 | cache=0 | ratio c1/c0 |")
print(f"|-------------|--------:|--------:|------------:|")
print(f"| zipf-0.99   | {z099_c1:7.2f} | {z099_c0:7.2f} |  {z099_c1/z099_c0:.3f}     |")
print(f"| zipf-1.5    | {z15_c1:7.2f} | {z15_c0:7.2f} |  {z15_c1/z15_c0:.3f}     |")
print(f"")
print(f"Cache collapse ratio (zipf-1.5 / zipf-0.99):")
print(f"  cache=1: {z15_c1/z099_c1:.3f}  (iter-15A baseline: 0.31)")
print(f"  cache=0: {z15_c0/z099_c0:.3f}  (anomaly survives w/o cache? key question)")
print(f"")
# Verdict logic
c1_collapse = z15_c1/z099_c1
c0_collapse = z15_c0/z099_c0
print(f"=== VERDICT ===")
if c0_collapse >= 0.7:
    print(f"→ cache=0 zipf-1.5 does NOT collapse ({c0_collapse:.2f} of zipf-0.99)")
    print(f"  CACHE PATH is the root cause of Anomaly B")
    print(f"  Hypothesis families to pursue: B-H1 (cpool_insert thundering)")
    print(f"                                  B-H2 (cacheline ping-pong)")
    print(f"                                  B-H4 (LRU evict storm)")
    print(f"  → Phase 2.2 (CAS retry counter) + Phase 2.4 stage decomp")
elif c0_collapse <= 0.5:
    print(f"→ cache=0 zipf-1.5 ALSO collapses ({c0_collapse:.2f} of zipf-0.99)")
    print(f"  HASHTABLE PATH is the root cause of Anomaly B (B-H3)")
    print(f"  Hot-key cacheline ping-pong on CXL bucket scan")
    print(f"  → Phase 2.4 stage decomp to confirm LRS3 (cxl_miss) dominates")
else:
    print(f"→ cache=0 zipf-1.5 partial collapse ({c0_collapse:.2f})")
    print(f"  BOTH cache + hashtable paths contribute")
    print(f"  → Run both Phase 2.2 and Phase 2.4 stage decomp")
PY
echo ""
echo "OUT: $OUT"
echo "CSV: $CSV"
