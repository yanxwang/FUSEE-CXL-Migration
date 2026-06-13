#!/usr/bin/env bash
# iter-19A Phase 1c — M1 (pre-touch) + M2 (madvise HUGEPAGE) sweep
# Test H8/H9: does pre-touching cache_pool pages in parent BEFORE fork
# close the c100/c1 gap? Also test M2 (explicit MADV_HUGEPAGE).
#
# 4 variants × 3 cache_pct × 3 reps = 36 cells, BD build (B-H3 + LRU removed).

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase1c_pretouch_$TS
mkdir -p $OUT/raw
CSV=$OUT/grid.csv

H0=g1
H1=g2
BUILD=build-cxl-w1-v1024-BD

declare -A CB=([1]=16384 [10]=131072 [100]=2097152)

echo "variant,pretouch,hugepage,cache_pct,cb,rep,thpt_Mops,r_avg_us,r_p50_us,r_p99_us,first_op_avg_us,first_op_max_us,ops_to_first_avg_us,r2hit,r2miss,trans_wall,wc" > $CSV

extract() { local v=$(grep -oP "^YCSB.*$2=\K[\d.]+" "$1" 2>/dev/null | head -1); [ -n "$v" ] && echo "$v" || echo "0"; }
sumagg() { local v0=$(grep "after_TRANS AGG" "$1" 2>/dev/null | head -1 | grep -oP "$3=\K[\d.]+" | head -1)
           local v1=$(grep "after_TRANS AGG" "$2" 2>/dev/null | head -1 | grep -oP "$3=\K[\d.]+" | head -1)
           awk -v a="${v0:-0}" -v b="${v1:-0}" 'BEGIN{print a+b}'; }

run_cell() {
  local variant=$1 pretouch=$2 hp=$3 cp=$4 rep=$5
  local cb=${CB[$cp]}
  local CK=$RANDOM$RANDOM
  local OH0=$OUT/raw/${variant}_c${cp}_rep${rep}_h0.out
  local OH1=$OUT/raw/${variant}_c${cp}_rep${rep}_h1.out

  ssh $H0 'pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9' >/dev/null 2>&1
  ssh $H1 'pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9' >/dev/null 2>&1
  sleep 0.5
  local T0=$(date +%s)
  timeout 120 ssh $H0 "cd /root/FUSEE_CXL/$BUILD && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=64 FUSEE_CACHE_PRETOUCH=$pretouch FUSEE_CACHE_HUGEPAGE=$hp FUSEE_RUN_COOKIE=$CK FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_WORKLOAD_NAME=local_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$cb ./tests/protocol_a_ycsb /dev/dax0.0 /tmp/microbench_traces/bench_local_read_zipf-0.99_h0.spec_load /tmp/microbench_traces/bench_local_read_zipf-0.99_h0.spec_trans 8388608 5000000 2>&1" > $OH0 2>&1 &
  local P0=$!
  timeout 120 ssh $H1 "cd /root/FUSEE_CXL/$BUILD && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=64 FUSEE_CACHE_PRETOUCH=$pretouch FUSEE_CACHE_HUGEPAGE=$hp FUSEE_RUN_COOKIE=$CK FUSEE_REP=$rep FUSEE_CACHE=1 FUSEE_WORKLOAD_NAME=local_read FUSEE_KV_SIZE=1024 FUSEE_CACHE_BUCKETS=$cb ./tests/protocol_a_ycsb /dev/dax0.0 /tmp/microbench_traces/bench_local_read_zipf-0.99_h1.spec_load /tmp/microbench_traces/bench_local_read_zipf-0.99_h1.spec_trans 8388608 5000000 2>&1" > $OH1 2>&1 &
  local P1=$!
  wait $P0 $P1
  local T1=$(date +%s); local wc=$((T1-T0))
  local thpt=$(extract $OH0 trans_agg_thpt)
  local r_avg_ns=$(extract $OH0 r_avg_ns)
  local r_p50_ns=$(extract $OH0 r_p50_ns)
  local r_p99_ns=$(extract $OH0 r_p99_ns)
  local first_op_max=$(extract $OH0 first_op_ns_max)
  local first_op_avg=$(extract $OH0 first_op_ns_avg)
  local ops_to_first_avg=$(extract $OH0 ops_to_first_ns_avg)
  local tw=$(extract $OH0 trans_wall_max)
  local r2h=$(sumagg $OH0 $OH1 r2hit)
  local r2m=$(sumagg $OH0 $OH1 r2miss_local)
  local mops=$(awk -v t=$thpt 'BEGIN{printf "%.3f", t/1e6}')
  local r_avg_us=$(awk -v n=${r_avg_ns:-0} 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p50_us=$(awk -v n=${r_p50_ns:-0} 'BEGIN{printf "%.3f", n/1000.0}')
  local r_p99_us=$(awk -v n=${r_p99_ns:-0} 'BEGIN{printf "%.3f", n/1000.0}')
  local fo_avg_us=$(awk -v n=${first_op_avg:-0} 'BEGIN{printf "%.1f", n/1000.0}')
  local fo_max_us=$(awk -v n=${first_op_max:-0} 'BEGIN{printf "%.1f", n/1000.0}')
  local of_avg_us=$(awk -v n=${ops_to_first_avg:-0} 'BEGIN{printf "%.1f", n/1000.0}')
  echo "$variant,$pretouch,$hp,$cp,$cb,$rep,$mops,$r_avg_us,$r_p50_us,$r_p99_us,$fo_avg_us,$fo_max_us,$of_avg_us,$r2h,$r2m,$tw,$wc" >> $CSV
  printf "  %-10s c%-3s rep=%s thpt=%6s Mops r_avg=%6s us r_p50=%5s us first_op_avg=%6s us\n" \
    "$variant" "$cp" "$rep" "$mops" "$r_avg_us" "$r_p50_us" "$fo_avg_us"
}

for cp in 1 10 100; do
  echo "=== cache_pct $cp ==="
  for rep in 1 2 3; do
    run_cell baseline 0 0 $cp $rep
    run_cell pretouch 1 0 $cp $rep
    run_cell HP_only  0 1 $cp $rep
    run_cell both     1 1 $cp $rep
  done
done

echo ""
echo "==== SUMMARY ===="
python3 <<PY
import csv, statistics
rows = list(csv.DictReader(open("$CSV")))
def med(sel, k): return statistics.median([float(r[k]) for r in sel]) if sel else 0

print(f"{'variant':<10}{'c%':>4}{'thpt Mops':>11}{'r_avg us':>10}{'r_p50 us':>10}{'r_p99 us':>10}{'first_op_avg us':>18}{'ops_to_first_avg us':>21}")
for v in ["baseline","pretouch","HP_only","both"]:
  for cp in ["1","10","100"]:
    sel=[r for r in rows if r["variant"]==v and r["cache_pct"]==cp]
    if not sel: continue
    print(f"{v:<10}{cp:>4}{med(sel,'thpt_Mops'):>11.2f}{med(sel,'r_avg_us'):>10.2f}{med(sel,'r_p50_us'):>10.2f}{med(sel,'r_p99_us'):>10.2f}{med(sel,'first_op_avg_us'):>18.1f}{med(sel,'ops_to_first_avg_us'):>21.1f}")
print()
print("=== Anomaly A magnitude (c100/c1) per variant ===")
for v in ["baseline","pretouch","HP_only","both"]:
  c1=med([r for r in rows if r['variant']==v and r['cache_pct']=='1'],'thpt_Mops')
  c100=med([r for r in rows if r['variant']==v and r['cache_pct']=='100'],'thpt_Mops')
  if c1>0: print(f"  {v}: c100/c1 = {c100/c1:.3f}  (c100 thpt={c100:.1f})")

print()
print("=== Long-tail ratio (r_avg / r_p50) per variant × cache_pct ===")
for v in ["baseline","pretouch","HP_only","both"]:
  for cp in ["1","10","100"]:
    sel=[r for r in rows if r["variant"]==v and r["cache_pct"]==cp]
    if not sel: continue
    avg=med(sel,'r_avg_us'); p50=med(sel,'r_p50_us')
    if p50>0: print(f"  {v}/c{cp}: r_avg/r_p50 = {avg/p50:.2f}x  (avg={avg:.2f}, p50={p50:.2f})")
PY
echo "OUT=$OUT"
