#!/usr/bin/env bash
# iter-19A Phase 0 baseline: reproduce iter-15A Phase 3 + Phase 4
# local_read cells on g1/g2 (CXL x8 era, post-2026-05-27 testbed).
#
# Grids (local_read scenario only):
#   Phase 3 grid: V=1024, T=64, zipf-0.99, cache_pct ∈ {1,2,5,10,20,50,100}, 3 reps
#   Phase 4 grid: V=1024, T=64, cache_pct=10, dist ∈ {uniform,zipf-0.5,zipf-0.99,zipf-1.5}, 3 reps
#
# HARD gate (QR3): compute g1/g2 reproduce ratios vs iter-15A
#   anomaly A ratio = thpt(cache_pct=100) / thpt(cache_pct=1)
#     iter-15A: 25.98 / 65.73 = 0.395
#   anomaly B ratio = thpt(zipf-1.5) / thpt(zipf-0.99)
#     iter-15A: 14.81 / 47.67 = 0.311
#   Verdict:
#     - both ≥ 0.6 → anomaly weakened, STOP iter-19A
#     - any in 0.3-0.6 → partial reproduction, continue Phase 1/2
#     - ≤ 0.3 → full reproduction, original plan

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase0_baseline_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

declare -A CACHE_BUCKETS=(
  [1]=16384
  [2]=32768
  [5]=65536
  [10]=131072
  [20]=262144
  [50]=524288
  [100]=2097152
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

echo "id,phase,V,T,cache_pct,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,r0_tls,r2hit,r2miss_local,r3,cpool_ins_r,cp_evict,cp_set_stale,cp_lru_evict,cache_filled,cache_fill_pct,wallclock_s" > $CSV

# Extract YCSB metric line value (e.g. trans_agg_thpt, r_p50_ns).
extract_metric() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep -oP "^YCSB.*$key=\K[\d.]+" "$file" 2>/dev/null | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}

# Extract after_TRANS AGG path counter from one host file.
extract_path_agg() {
  local file=$1 key=$2 dflt=$3
  local v=$(grep "after_TRANS AGG" "$file" 2>/dev/null | head -1 | grep -oP "$key=\K[\d.]+" | head -1)
  [ -n "$v" ] && echo "$v" || echo "$dflt"
}

# Sum h0 + h1 path counters.
sum_path_agg() {
  local f0=$1 f1=$2 key=$3
  local v0=$(extract_path_agg "$f0" "$key" 0)
  local v1=$(extract_path_agg "$f1" "$key" 0)
  awk -v a=$v0 -v b=$v1 'BEGIN{print a+b}'
}

run_cell() {
  local phase=$1 id=$2 cache_pct=$3 keydist=$4 rep=$5
  local cb=${CACHE_BUCKETS[$cache_pct]}
  local cookie=$RANDOM$RANDOM
  local t0=$(date +%s)
  local trace_h0=bench_local_read_${keydist}_h0
  local trace_h1=bench_local_read_${keydist}_h1
  local out_h0=$OUT/raw/${id}_rep${rep}_h0.out
  local out_h1=$OUT/raw/${id}_rep${rep}_h1.out

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9" >/dev/null 2>&1
  done
  sleep 1

  timeout 180 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h0}.spec_load $TRACE/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h0 2>&1 &
  pid0=$!
  timeout 180 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=local_read \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/${trace_h1}.spec_load $TRACE/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1" > $out_h1 2>&1 &
  pid1=$!
  wait $pid0
  wait $pid1
  local t1=$(date +%s)
  local wc=$((t1-t0))

  # Aggregate thpt = host 0's reported trans_agg_thpt (cluster level)
  local thpt=$(extract_metric $out_h0 'trans_agg_thpt' 0)
  local r_p50_ns=$(extract_metric $out_h0 'r_p50_ns' 0)
  local r_p99_ns=$(extract_metric $out_h0 'r_p99_ns' 0)
  local r_p50_us=$(awk -v n=$r_p50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99_us=$(awk -v n=$r_p99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  # Path counter AGG line (sum of h0 + h1, after_TRANS only)
  local r0_tls=$(sum_path_agg $out_h0 $out_h1 'r0_tls')
  local r2hit=$(sum_path_agg $out_h0 $out_h1 'r2hit')
  local r2miss=$(sum_path_agg $out_h0 $out_h1 'r2miss_local')
  local r3=$(sum_path_agg $out_h0 $out_h1 'r3')
  local cpool_ins_r=$(sum_path_agg $out_h0 $out_h1 'cpool_ins_r')
  local cp_evict=$(sum_path_agg $out_h0 $out_h1 'cp_evict')
  local cp_set_stale=$(sum_path_agg $out_h0 $out_h1 'cp_set_stale')
  local cp_lru_evict=$(sum_path_agg $out_h0 $out_h1 'cp_lru_evict')
  # cache_filled / cache_fill_pct: report from h0
  local cache_filled=$(grep "cache_filled" $out_h0 2>/dev/null | grep -oP 'cache_filled=\K[\d.]+' | head -1)
  local cache_fill_pct=$(grep "cache_fill_pct" $out_h0 2>/dev/null | grep -oP 'cache_fill_pct=\K[\d.]+' | head -1)
  [ -z "$cache_filled" ] && cache_filled=0
  [ -z "$cache_fill_pct" ] && cache_fill_pct=0

  local thpt_mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')
  echo "$id,$phase,$V,$T,$cache_pct,$cb,local_read,$keydist,$rep,$thpt_mops,$r_p50_us,$r_p99_us,$r0_tls,$r2hit,$r2miss,$r3,$cpool_ins_r,$cp_evict,$cp_set_stale,$cp_lru_evict,$cache_filled,$cache_fill_pct,$wc" >> $CSV
  echo "  $id rep=$rep thpt=$thpt_mops Mops r_p50=${r_p50_us}us wc=${wc}s"
}

echo "==== Phase 0.2 — cache_pct sweep (Anomaly A) ===="
for cache in 1 2 5 10 20 50 100; do
  for rep in 1 2 3; do
    run_cell phase3 "ph0a_V${V}_T${T}_c${cache}_local_read_zipf-0.99" $cache zipf-0.99 $rep
  done
done

echo "==== Phase 0.3 — dist sweep (Anomaly B) ===="
for dist in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
  for rep in 1 2 3; do
    run_cell phase4 "ph0b_V${V}_T${T}_c10_local_read_${dist}" 10 $dist $rep
  done
done

echo ""
echo "==== Phase 0.4 — reproduce gate ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
def med(key, val, fld="thpt_Mops"):
    sel = [float(r[fld]) for r in rows if r[key] == val]
    return statistics.median(sel) if sel else 0.0
def med2(filters, fld="thpt_Mops"):
    sel = [float(r[fld]) for r in rows if all(r[k]==v for k,v in filters.items())]
    return statistics.median(sel) if sel else 0.0

a_c1 = med2({"cache_pct":"1", "keydist":"zipf-0.99"})
a_c100 = med2({"cache_pct":"100", "keydist":"zipf-0.99"})
b_z099 = med2({"keydist":"zipf-0.99", "cache_pct":"10"})
b_z15 = med2({"keydist":"zipf-1.5", "cache_pct":"10"})

ratio_A = a_c100 / a_c1 if a_c1 > 0 else 0
ratio_B = b_z15 / b_z099 if b_z099 > 0 else 0

print(f"=== HARD gate (QR3) ===")
print(f"")
print(f"Anomaly A — cache_pct=100 / cache_pct=1 thpt ratio:")
print(f"  iter-15A baseline: 25.98 / 65.73 = 0.395")
print(f"  g1/g2 today:       {a_c100:.2f} / {a_c1:.2f} = {ratio_A:.3f}")
print(f"")
print(f"Anomaly B — zipf-1.5 / zipf-0.99 thpt ratio:")
print(f"  iter-15A baseline: 14.81 / 47.67 = 0.311")
print(f"  g1/g2 today:       {b_z15:.2f} / {b_z099:.2f} = {ratio_B:.3f}")
print(f"")
verdicts = []
for name, r, ref in [("A", ratio_A, 0.395), ("B", ratio_B, 0.311)]:
    if r <= 0.3:
        v = "FULL reproduction"
    elif r <= 0.6:
        v = "PARTIAL reproduction"
    else:
        v = "WEAKENED / NOT reproduce"
    print(f"Anomaly {name}: ratio {r:.3f} → {v}")
    verdicts.append((name, v))

print(f"")
all_weak = all("WEAKENED" in v for _, v in verdicts)
if all_weak:
    print(f"→ STOP iter-19A anomaly study, rewrite as platform-sensitivity report")
else:
    print(f"→ CONTINUE to Phase 1/2")

# Save verdict to file
with open("$OUT/reproduce_check.md", "w") as f:
    f.write("# iter-19A Phase 0 baseline reproduction\n\n")
    f.write(f"Date: $(date)\n")
    f.write(f"Platform: g1+g2 (CXL x8 era, post-2026-05-27)\n")
    f.write(f"Reference: iter-15A Phase 3+4 on g3+g4 (CXL x16 era)\n\n")
    f.write(f"## Anomaly A (cache_pct sweep)\n\n")
    f.write(f"| cache_pct | g1/g2 thpt (Mops) | iter-15A thpt (Mops) |\n")
    f.write(f"|---:|---:|---:|\n")
    iter15A_a = {"1":65.73, "2":62.44, "5":55.88, "10":47.55, "20":38.52, "50":32.06, "100":25.98}
    for c in ["1","2","5","10","20","50","100"]:
        t = med2({"cache_pct":c, "keydist":"zipf-0.99"})
        f.write(f"| {c} | {t:.2f} | {iter15A_a[c]:.2f} |\n")
    f.write(f"\nRatio cache_pct=100/cache_pct=1: g1/g2 = **{ratio_A:.3f}** vs iter-15A 0.395\n\n")
    f.write(f"## Anomaly B (dist sweep, cache_pct=10)\n\n")
    f.write(f"| dist | g1/g2 thpt (Mops) | iter-15A thpt (Mops) |\n")
    f.write(f"|---|---:|---:|\n")
    iter15A_b = {"uniform":29.52, "zipf-0.5":40.52, "zipf-0.99":47.67, "zipf-1.5":14.81}
    for d in ["uniform","zipf-0.5","zipf-0.99","zipf-1.5"]:
        t = med2({"keydist":d, "cache_pct":"10"})
        f.write(f"| {d} | {t:.2f} | {iter15A_b[d]:.2f} |\n")
    f.write(f"\nRatio zipf-1.5/zipf-0.99: g1/g2 = **{ratio_B:.3f}** vs iter-15A 0.311\n\n")
    f.write(f"## Verdict\n\n")
    for name, v in verdicts:
        f.write(f"- Anomaly {name}: {v}\n")
    if all_weak:
        f.write("\n**→ STOP iter-19A anomaly study, rewrite as platform-sensitivity report**\n")
    else:
        f.write("\n**→ CONTINUE to Phase 1 (cache size anomaly) + Phase 2 (zipf-1.5 anomaly)**\n")
print(f"")
print(f"reproduce_check.md written to: $OUT/reproduce_check.md")
PY

echo ""
echo "OUT: $OUT"
echo "CSV: $CSV"
