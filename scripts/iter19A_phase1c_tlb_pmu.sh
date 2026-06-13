#!/usr/bin/env bash
# iter-19A Phase 1c — TLB miss + PT walk PMU measurement
# Direct verification of E1 (TLB miss rate) and E2 (PT walk DRAM hit) hypotheses.
#
# Runs synth pthread vs synth_fork, at c1 vs c100, T=64, with perf stat events:
#   stlb_hit_loads, stlb_miss_loads  → TLB miss rate per load
#   dtlb_load_misses.walk_completed   → number of PT walks
#   dtlb_load_misses.walk_completed_{4k,2m_4m,1g}  → page granularity check
#   dtlb_load_misses.walk_active      → cycles spent in PT walks
#   dtlb_load_misses.stlb_hit         → dTLB-miss → STLB-hit (no walk)
#
# Derived:
#   walks_per_op = walk_completed / total_ops
#   cycles_per_walk = walk_active / walk_completed
#   ns_per_walk = cycles_per_walk * (1/clock_GHz)
#   tlb_miss_rate = stlb_miss_loads / (stlb_hit_loads + stlb_miss_loads)

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/docs/iter19A_phase1c_tlb_pmu_$TS
mkdir -p $OUT
CSV=$OUT/results.csv
echo "model,cache_pct,T,ops_total,wall_s,thpt_Mops,cycles,instructions,IPC,stlb_hit,stlb_miss,tlb_miss_rate,walk_completed,walk_4k,walk_2m,walk_active_cycles,cycles_per_walk,ns_per_walk_est,walks_per_op,walk_time_pct" > $CSV

# We use ~3.0 GHz as estimate; refined later from cycles/wall_s
GHZ_EST=3.0

PMU="cycles,instructions,mem_inst_retired.stlb_hit_loads,mem_inst_retired.stlb_miss_loads,dtlb_load_misses.walk_completed,dtlb_load_misses.walk_completed_4k,dtlb_load_misses.walk_completed_2m_4m,dtlb_load_misses.walk_active,dtlb_load_misses.stlb_hit"

run_perf() {
  local model=$1 cp=$2 T=$3 rep=$4
  declare -A CB=([1]=16384 [100]=2097152)
  local cb=${CB[$cp]}
  local ops_per_proc=$((12800000 / T))  # 12.8M total ops, split across T workers
  local prog
  if [ "$model" = "pthread" ]; then
    prog="/tmp/synth $cb $T $ops_per_proc"
  else
    prog="/tmp/synth_fork $cb $T $ops_per_proc"
  fi
  local pf=$OUT/perf_${model}_c${cp}_T${T}_rep${rep}.txt

  # perf stat with -x ',' for CSV; --inherit (default) tracks children
  ssh g1 "perf stat -e $PMU -x ',' -- $prog 2>&1" > $pf 2>&1

  # Parse perf output (events come as CSV: count,unit,name,run_time,pct,...)
  # Use python for robust parsing
  parsed=$(python3 - "$pf" <<'EOF'
import sys, re
data = open(sys.argv[1]).read()
def get(name):
    pat = r'^(\d+),[^,]*,' + re.escape(name) + r','
    m = re.search(pat, data, re.MULTILINE)
    return int(m.group(1)) if m else 0
events = ["cycles","instructions","mem_inst_retired.stlb_hit_loads",
          "mem_inst_retired.stlb_miss_loads","dtlb_load_misses.walk_completed",
          "dtlb_load_misses.walk_completed_4k","dtlb_load_misses.walk_completed_2m_4m",
          "dtlb_load_misses.walk_active"]
print(",".join(str(get(e)) for e in events))
EOF
  )
  IFS=',' read -r cycles insns stlb_hit stlb_miss walks walks4k walks2m walk_active <<<"$parsed"

  # Parse SYNTH line for ops + wall
  local synth_line=$(grep -E "^SYNTH" "$pf")
  local total_ops=$(echo "$synth_line" | grep -oP "total_ops=\K\d+")
  local wall_s=$(echo "$synth_line" | grep -oP "wall=\K[\d.]+")
  local thpt=$(echo "$synth_line" | grep -oP "aggr_thpt=\K[\d.]+")
  total_ops=${total_ops:-0}; wall_s=${wall_s:-1}; thpt=${thpt:-0}

  # Derived metrics (use python — awk's > inside print is redirection, not gt)
  local derived=$(python3 - <<EOF
i,c,sh,sm,w,wa,o,ws=$insns,$cycles,$stlb_hit,$stlb_miss,$walks,$walk_active,$total_ops,$wall_s
ipc=i/c if c>0 else 0
total=sh+sm
miss_rate=sm/total*100 if total>0 else 0
cyc_per_walk=wa/w if w>0 else 0
ghz=c/(ws*1e9) if ws>0 else 3.0
ns_per_walk=cyc_per_walk/ghz if ghz>0 else 0
walks_per_op=w/o if o>0 else 0
walk_pct=wa/c*100 if c>0 else 0
print(f"{ipc:.3f},{miss_rate:.3f},{cyc_per_walk:.1f},{ghz:.3f},{ns_per_walk:.1f},{walks_per_op:.5f},{walk_pct:.2f}")
EOF
  )
  IFS=',' read -r ipc miss_rate cyc_per_walk ghz ns_per_walk walks_per_op walk_pct <<<"$derived"

  echo "$model,$cp,$T,$total_ops,$wall_s,$thpt,$cycles,$insns,$ipc,$stlb_hit,$stlb_miss,$miss_rate,$walks,$walks4k,$walks2m,$walk_active,$cyc_per_walk,$ns_per_walk,$walks_per_op,$walk_pct" >> $CSV
  printf "  %-7s c%-3s T=%s thpt=%s Mops freq=%.2f GHz | TLBmiss=%.2f%% walks/op=%.3f cyc/walk=%.1f ns/walk=%.1f walk_%%=%.1f%%\n" \
    "$model" "$cp" "$T" "$thpt" "$ghz" "$miss_rate" "$walks_per_op" "$cyc_per_walk" "$ns_per_walk" "$walk_pct"
}

echo "==== TLB + PT walk PMU sweep ===="
echo "Hypothesis E1: c100 high TLB miss rate (working set > STLB), c1 low"
echo "Hypothesis E2: fork c100 has high cycles_per_walk (DRAM hit), pthread c100 low (L3 hit)"
echo ""
for cp in 1 100; do
  for T in 1 64; do
    for model in pthread fork; do
      for rep in 1 2 3; do
        run_perf $model $cp $T $rep
      done
    done
  done
done

echo ""
echo "==== ANALYSIS ===="
python3 <<PY
import csv, statistics
rows=list(csv.DictReader(open("$CSV")))
def med(sel,k): return statistics.median([float(r[k]) for r in sel]) if sel else 0

print()
print("--- E1: TLB miss rate (% of loads that miss STLB) ---")
print(f"{'model':<8}{'c%':>5}{'T':>4}{'TLB miss%':>12}{'walks/op':>11}{'4K walks%':>12}{'2M walks%':>12}")
for model in ["pthread","fork"]:
  for cp in ["1","100"]:
    for T in ["1","64"]:
      sel=[r for r in rows if r['model']==model and r['cache_pct']==cp and r['T']==T]
      if not sel: continue
      w4k=med(sel,'walk_4k'); w2m=med(sel,'walk_2m'); wt=w4k+w2m
      pct_4k = w4k/wt*100 if wt>0 else 0
      pct_2m = w2m/wt*100 if wt>0 else 0
      print(f"{model:<8}{cp:>5}{T:>4}{med(sel,'tlb_miss_rate'):>11.2f}%{med(sel,'walks_per_op'):>11.4f}{pct_4k:>11.1f}%{pct_2m:>11.1f}%")

print()
print("--- E2: cycles/walk and ns/walk (= PT walk DRAM hit signature) ---")
print(f"{'model':<8}{'c%':>5}{'T':>4}{'cyc/walk':>10}{'ns/walk':>9}{'walk %cycles':>14}")
for model in ["pthread","fork"]:
  for cp in ["1","100"]:
    for T in ["1","64"]:
      sel=[r for r in rows if r['model']==model and r['cache_pct']==cp and r['T']==T]
      if not sel: continue
      print(f"{model:<8}{cp:>5}{T:>4}{med(sel,'cycles_per_walk'):>10.1f}{med(sel,'ns_per_walk_est'):>9.1f}{med(sel,'walk_time_pct'):>13.1f}%")

print()
print("--- E2 KEY COMPARISON: pthread c100 vs fork c100 at T=64 ---")
pt=[r for r in rows if r['model']=='pthread' and r['cache_pct']=='100' and r['T']=='64']
fk=[r for r in rows if r['model']=='fork' and r['cache_pct']=='100' and r['T']=='64']
if pt and fk:
  pt_ns=med(pt,'ns_per_walk_est'); fk_ns=med(fk,'ns_per_walk_est')
  pt_pct=med(pt,'walk_time_pct'); fk_pct=med(fk,'walk_time_pct')
  print(f"  pthread c100 T=64: {pt_ns:.1f} ns/walk, {pt_pct:.1f}% time in walks")
  print(f"  fork    c100 T=64: {fk_ns:.1f} ns/walk, {fk_pct:.1f}% time in walks")
  if pt_ns>0: print(f"  fork ÷ pthread cycles/walk = {fk_ns/pt_ns:.2f}x")
  print()
  if fk_ns/pt_ns > 3:
    print("  → fork walks are >3x slower → CONSISTENT with DRAM-hit hypothesis ✓")
  elif fk_ns/pt_ns > 1.5:
    print("  → fork walks moderately slower → mixed L3/DRAM hits or partial confirmation")
  else:
    print("  → fork walks similar to pthread → DRAM hit hypothesis FALSIFIED, mechanism is elsewhere")
PY
echo ""
echo "OUT=$OUT"
