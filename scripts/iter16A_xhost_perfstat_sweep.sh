#!/usr/bin/env bash
# iter-16A xhost_write T-sweep with perf stat (probe-OFF).
#
# Same params as iter16A_xhost_T_sweep.sh but adds `perf stat` on each
# host capturing: cycles, instructions, cache-misses, LLC-loads,
# LLC-load-misses, l1-dcache-load-misses.
#
# Produces grid.csv (thpt + perf event counts) + summary plots.

set -u
ROOT=/home/yanwang/FUSEE
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$ROOT/docs/iter16A_xhost_perfstat_${TS}"
mkdir -p "$OUT_BASE/raw"

DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=5000000
V=1024
CACHE_BUCKETS=131072
BUILD="build-cxl-w1-v${V}"   # PROBE-OFF for clean perf measurement
KD="zipf-0.99"
SCEN="xhost_write"
TS_LIST="1 2 4 8 16 32 64"
TRACE=/root/FUSEE_CXL/setup/iter15A_microbench_traces
EVENTS="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses,l1d.replacement"

CSV="$OUT_BASE/grid.csv"
echo "scenario,T,rep,thpt_Mops,r_p50_us,r_p99_us,w_p50_us,w_p99_us,cycles,instructions,cache_misses,llc_loads,llc_load_misses,l1d_replacement,wallclock_s" > "$CSV"

run_cell() {
  local T="$1" rep="$2"
  local cookie=$RANDOM$RANDOM
  local id="ph_T${T}_${SCEN}_rep${rep}"
  local out_h0="$OUT_BASE/raw/${id}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_h1.out"
  local perf_h0="$OUT_BASE/raw/${id}_h0.perf"
  local perf_h1="$OUT_BASE/raw/${id}_h1.perf"
  local t_start=$(date +%s)

  ssh root@g3 "
    cd /root/FUSEE_CXL/${BUILD}
    perf stat -e $EVENTS -o /tmp/perf_$cookie.txt -- \
    env FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h0.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
    cat /tmp/perf_$cookie.txt
  " > "$out_h0" 2>&1 &
  ssh root@g4 "
    cd /root/FUSEE_CXL/${BUILD}
    perf stat -e $EVENTS -o /tmp/perf_$cookie.txt -- \
    env FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$SCEN FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$CACHE_BUCKETS \
    ./tests/protocol_a_ycsb $DEV \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_load \
      $TRACE/bench_${SCEN}_${KD}_h1.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
    cat /tmp/perf_$cookie.txt
  " > "$out_h1" 2>&1 &
  wait
  local t_end=$(date +%s)
  local wall=$((t_end - t_start))

  local thpt=$(grep -oP 'trans_agg_thpt=\K[0-9]+' "$out_h0" | head -1)
  [[ -z "$thpt" ]] && thpt=0
  local thpt_M=$(awk -v a=$thpt 'BEGIN{printf "%.3f", a/1e6}')

  local rp50_ns=$(grep -oP 'r_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$rp50_ns" ]] && rp50_ns=0
  local rp99_ns=$(grep -oP 'r_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$rp99_ns" ]] && rp99_ns=0
  local wp50_ns=$(grep -oP 'w_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$wp50_ns" ]] && wp50_ns=0
  local wp99_ns=$(grep -oP 'w_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$wp99_ns" ]] && wp99_ns=0
  local rp50=$(awk -v n=$rp50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local rp99=$(awk -v n=$rp99_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local wp50=$(awk -v n=$wp50_ns 'BEGIN{printf "%.3f", n/1000.0}')
  local wp99=$(awk -v n=$wp99_ns 'BEGIN{printf "%.3f", n/1000.0}')

  # Parse perf stat output (last lines of h0)
  local cycles=$(grep -oP '^\s*[\d,]+(?=\s+cycles)' "$out_h0" | tail -1 | tr -d ',')
  local instr=$(grep -oP '^\s*[\d,]+(?=\s+instructions)' "$out_h0" | tail -1 | tr -d ',')
  local cmiss=$(grep -oP '^\s*[\d,]+(?=\s+cache-misses)' "$out_h0" | tail -1 | tr -d ',')
  local llcl=$(grep -oP '^\s*[\d,]+(?=\s+LLC-loads)' "$out_h0" | tail -1 | tr -d ',')
  local llclm=$(grep -oP '^\s*[\d,]+(?=\s+LLC-load-misses)' "$out_h0" | tail -1 | tr -d ',')
  local l1dr=$(grep -oP '^\s*[\d,]+(?=\s+l1d\.replacement)' "$out_h0" | tail -1 | tr -d ',')
  cycles=${cycles:-0}; instr=${instr:-0}; cmiss=${cmiss:-0}
  llcl=${llcl:-0}; llclm=${llclm:-0}; l1dr=${l1dr:-0}

  echo "$SCEN,$T,$rep,$thpt_M,$rp50,$rp99,$wp50,$wp99,$cycles,$instr,$cmiss,$llcl,$llclm,$l1dr,$wall" >> "$CSV"
  echo "  [done] T=$T rep=$rep thpt=$thpt_M Mops cycles=$cycles wall=${wall}s"
}

# Preflight
for h in g3 g4; do
  n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v pgrep | wc -l")
  if [[ "$n" != "0" ]]; then echo "[PREFLIGHT] host=$h has $n procs, aborting" >&2; exit 1; fi
  ssh root@$h "which perf >/dev/null 2>&1 || (apt-get install -y linux-tools-common linux-tools-\$(uname -r) 2>/dev/null || true)"
done

echo "[perfstat-sweep] T_list=$TS_LIST reps=3"
for T in $TS_LIST; do
  for rep in 1 2 3; do
    run_cell $T $rep
    sleep 1
  done
done

echo "==done=="
echo "OUT: $OUT_BASE"
echo "rows: $(grep -c '^' $CSV)"
