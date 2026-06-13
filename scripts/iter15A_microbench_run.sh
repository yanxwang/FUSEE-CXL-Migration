#!/usr/bin/env bash
# iter-15A microbench driver. Per-cell runner that distributes work across
# g3 + g4 in parallel and writes raw outputs + per-cell CSV row.
#
# Usage:
#   scripts/iter15A_microbench_run.sh phase0
#   scripts/iter15A_microbench_run.sh phase1
#   ... up to phase5_ext
#
# Per-cell schema:
#   id, phase, V, T, cache_pct, num_cache_buckets, scenario, keydist, rep,
#   trans_thpt_Mops, r_p50_us, r_p99_us, w_p50_us, w_p99_us,
#   r0_tls, r2hit, r2miss_local, r3, cpool_ins_r,
#   local_w, fwd_w, local_w_blk_fwd, local_w_stg_fwd, cpool_ins_w,
#   cp_evict, cp_set_stale, cp_lru_evict, cache_filled, cache_fill_pct

set -u

ROOT=/home/yanwang/FUSEE
TRACE_DIR=$ROOT/setup/iter15A_microbench_traces
TS=$(date +%Y%m%d_%H%M%S)
PHASE="${1:-}"
OUT_BASE="$ROOT/docs/iter15A_microbench_${PHASE}_${TS}"
mkdir -p "$OUT_BASE/raw"

CSV="$OUT_BASE/grid.csv"
echo "id,phase,V,T,cache_pct,cache_buckets,scenario,keydist,rep,thpt_Mops,r_p50_us,r_p99_us,w_p50_us,w_p99_us,r0_tls,r2hit,r2miss_local,r3,cpool_ins_r,local_w,fwd_w,local_w_blk_fwd,local_w_stg_fwd,cpool_ins_w,cp_evict,cp_set_stale,cp_lru_evict,rh_served,cache_filled,cache_fill_pct,wallclock_s" > "$CSV"

# HR-1/HR-2 thresholds.
# - xhost_WRITE: every UPDATE on peer-key always forwards (no cache shortcut),
#                so lw_blk_fwd should equal cluster_trans ± 10%.
# - xhost_READ: cache hits skip forward, so rh_served is BELOW trans by hit_rate.
#               At cache=100% steady-state, hit_rate→100% and rh_served can be
#               very small. Lower-bound only (>5% of trans means routing works);
#               upper bound = trans × 1.1 sanity.
TOTAL_TRANS_OPS_CLUSTER=5000000
HR2_W_LO=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 0.9)}')
HR2_W_HI=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 1.1)}')
HR2_R_LO=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 0.05)}')
HR2_R_HI=$(awk -v t=$TOTAL_TRANS_OPS_CLUSTER 'BEGIN{print int(t * 1.1)}')

# cache% → cache_buckets (closest power-of-2). entries_per_bucket=4, 5M keys/host.
declare -A CACHE_BUCKETS=(
  [1]=16384         # 64K entries  =  1.28% of 5M
  [2]=32768         # 128K         =  2.56%
  [5]=65536         # 256K         =  5.12%
  [10]=131072       # 512K         = 10.24%
  [20]=262144       # 1M           = 20.48%
  [50]=524288       # 2M           = 40.96% (closest pow2)
  [100]=2097152     # 8M           = 163.84% (overprovisioned for full coverage)
)

NUM_BUCKETS=8388608   # 8M data hashtable buckets (handle 10M unique keys w/ headroom)
TRANS_OPS=5000000     # per host
DEV=/dev/dax0.0

# Wait until both hosts are clean of stale protocol_a_ycsb procs.
# (Filter grep/pgrep's own command line.)
preflight() {
  for h in g3 g4; do
    local n
    n=$(ssh root@$h "pgrep -af protocol_a_ycsb 2>/dev/null | grep -v 'pgrep' | wc -l" 2>/dev/null)
    if [[ "$n" != "0" ]]; then
      echo "[PREFLIGHT] host=$h has $n procs, aborting" >&2
      ssh root@$h "pgrep -af protocol_a_ycsb | grep -v 'pgrep'" >&2 || true
      return 1
    fi
  done
  return 0
}

# Run one cell (one rep). Drives both hosts in parallel via ssh.
# Args: cell_id V T cache_pct scenario keydist rep build_dir trace_h0 trace_h1
run_cell() {
  local id="$1" V="$2" T="$3" cache_pct="$4" scenario="$5" keydist="$6" rep="$7"
  local build="$8" trace_h0="$9" trace_h1="${10}"

  local cb="${CACHE_BUCKETS[$cache_pct]}"
  local cookie=$RANDOM
  local out_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.out"
  local out_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.out"
  local err_h0="$OUT_BASE/raw/${id}_rep${rep}_h0.err"
  local err_h1="$OUT_BASE/raw/${id}_rep${rep}_h1.err"

  local t_start=$(date +%s)

  # Host 0 (g3) & Host 1 (g4) in parallel.
  ssh root@g3 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$scenario \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/${trace_h0}.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/${trace_h0}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h0" 2> "$err_h0" &
  local pid_g3=$!

  ssh root@g4 "
    cd /root/FUSEE_CXL/${build}
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=1 \
    FUSEE_WORKLOAD_NAME=$scenario \
    FUSEE_KV_SIZE=$V FUSEE_CACHE_BUCKETS=$cb \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/${trace_h1}.spec_load \
      /root/FUSEE_CXL/setup/iter15A_microbench_traces/${trace_h1}.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1
  " > "$out_h1" 2> "$err_h1" &
  local pid_g4=$!

  wait $pid_g3
  wait $pid_g4

  local t_end=$(date +%s)
  local wallclock=$((t_end - t_start))

  # Parse host 0 output for the metric line + path counter AGG.
  parse_cell_to_csv "$id" "$V" "$T" "$cache_pct" "$cb" "$scenario" "$keydist" "$rep" \
                    "$out_h0" "$out_h1" "$wallclock"
}

# Aggregate thpt across both hosts. r/w_p50/p99 from host 0.
# Path counters: sum AGG line from both hosts.
parse_cell_to_csv() {
  local id="$1" V="$2" T="$3" cp="$4" cb="$5" sc="$6" kd="$7" rep="$8"
  local out_h0="$9" out_h1="${10}" wallclock="${11}"

  # Each host prints one line like:
  #   "# <wl>_optA_t<T>_cache<on|off>_rep<R>" with throughput in a previous line.
  # Look at the metrics line from host 0 (host 0 = primary, sees aggregate by setup):
  # Format expected (matches scaling_ycsb / protocol_a_ycsb output):
  #   "trans_agg_kops" field maps to thpt (kops/s, aggregate across hosts).
  # Use the simpler trans_thpt= line if present (Mops/s).

  # protocol_a_ycsb prints `trans_agg_thpt=<ops/s>` (despite var-name-suggested kops)
  # and latencies in ns. Aggregate thpt across hosts = sum(host0, host1).
  local thpt0 thpt1 r_p50_ns r_p99_ns w_p50_ns w_p99_ns
  thpt0=$(grep -oP 'trans_agg_thpt=\K[0-9.]+' "$out_h0" | head -1); [[ -z "$thpt0" ]] && thpt0=0
  thpt1=$(grep -oP 'trans_agg_thpt=\K[0-9.]+' "$out_h1" | head -1); [[ -z "$thpt1" ]] && thpt1=0
  # ops/s + ops/s → Mops/s = (sum) / 1e6
  local thpt_M
  thpt_M=$(awk -v a="$thpt0" -v b="$thpt1" 'BEGIN{printf "%.3f", (a+b)/1e6}')

  r_p50_ns=$(grep -oP 'r_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p50_ns" ]] && r_p50_ns=0
  r_p99_ns=$(grep -oP 'r_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$r_p99_ns" ]] && r_p99_ns=0
  w_p50_ns=$(grep -oP 'w_p50_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p50_ns" ]] && w_p50_ns=0
  w_p99_ns=$(grep -oP 'w_p99_ns=\K[0-9]+' "$out_h0" | head -1); [[ -z "$w_p99_ns" ]] && w_p99_ns=0
  # Convert ns → µs.
  local r_p50 r_p99 w_p50 w_p99
  r_p50=$(awk -v n="$r_p50_ns" 'BEGIN{printf "%.3f", n/1000.0}')
  r_p99=$(awk -v n="$r_p99_ns" 'BEGIN{printf "%.3f", n/1000.0}')
  w_p50=$(awk -v n="$w_p50_ns" 'BEGIN{printf "%.3f", n/1000.0}')
  w_p99=$(awk -v n="$w_p99_ns" 'BEGIN{printf "%.3f", n/1000.0}')

  # Path counters from AGG lines (one per host).
  # Format (post bimodal debug): "# PATH host=N label=after_TRANS AGG ..."
  local pc_line_h0 pc_line_h1
  pc_line_h0=$(grep "^# PATH host=0 label=after_TRANS AGG" "$out_h0" | tail -1)
  pc_line_h1=$(grep "^# PATH host=1 label=after_TRANS AGG" "$out_h1" | tail -1)

  # Extract fields. Format: "r0_tls=N r2hit=N ..." etc.
  declare -A pc
  for k in r0_tls r2hit r2miss_local r3 cpool_ins_r local_w fwd_w \
           local_w_blk_fwd local_w_stg_fwd cpool_ins_w \
           cp_evict cp_set_stale cp_lru_evict rh_served; do
    local v0=0 v1=0
    if [[ -n "$pc_line_h0" ]]; then
      v0=$(echo "$pc_line_h0" | grep -oP "$k=\K[0-9]+" | head -1)
      [[ -z "$v0" ]] && v0=0
    fi
    if [[ -n "$pc_line_h1" ]]; then
      v1=$(echo "$pc_line_h1" | grep -oP "$k=\K[0-9]+" | head -1)
      [[ -z "$v1" ]] && v1=0
    fi
    pc[$k]=$((v0 + v1))
  done

  # Cache fill (host 0 only).
  local cache_filled=0 cache_fill_pct=0
  local cf_line
  cf_line=$(grep "^# CACHE_FILL host=0" "$out_h0" | tail -1)
  if [[ -n "$cf_line" ]]; then
    cache_filled=$(echo "$cf_line" | grep -oP 'filled=\K[0-9]+' | head -1)
    cache_fill_pct=$(echo "$cf_line" | grep -oP 'fill_pct=\K[0-9.]+' | head -1)
    [[ -z "$cache_filled" ]] && cache_filled=0
    [[ -z "$cache_fill_pct" ]] && cache_fill_pct=0
  fi

  echo "$id,$PHASE,$V,$T,$cp,$cb,$sc,$kd,$rep,$thpt_M,$r_p50,$r_p99,$w_p50,$w_p99,${pc[r0_tls]},${pc[r2hit]},${pc[r2miss_local]},${pc[r3]},${pc[cpool_ins_r]},${pc[local_w]},${pc[fwd_w]},${pc[local_w_blk_fwd]},${pc[local_w_stg_fwd]},${pc[cpool_ins_w]},${pc[cp_evict]},${pc[cp_set_stale]},${pc[cp_lru_evict]},${pc[rh_served]},$cache_filled,$cache_fill_pct,$wallclock" >> "$CSV"

  echo "  [done] $id rep=$rep thpt=${thpt_M} Mops wallclock=${wallclock}s"

  # ===== HR-2 (per-cell counter gate) =====
  # Counters in pc[*] are cluster sums (h0 AGG + h1 AGG).
  hr2_check "$id" "$sc" "${pc[lw_blk_fwd_total]:-${pc[local_w_blk_fwd]}}" "${pc[rh_served]}" "${pc[fwd_w]}" "${pc[r3]}" || {
    echo "[HR-2 FAIL] $id — phase aborting (see message above)"
    exit 90
  }

  # ===== HR-1 (online local > xhost throughput) =====
  hr1_check "$V" "$T" "$cp" "$kd" "$sc" "$thpt_M" || {
    echo "[HR-1 FAIL] phase aborting (see message above)"
    exit 91
  }
}

# HR-2: routing/counter gate. local_X must have ~zero forward activity;
# xhost_X must have ~all-cluster forward activity (lw_blk_fwd or rh_served).
# Args: id, scenario, lw_blk_fwd (cluster), rh_served (cluster), fwd_w (primary), r3 (primary)
hr2_check() {
  local id="$1" sc="$2" lwbf="$3" rh="$4" fwd_w="$5" r3="$6"
  case "$sc" in
    local_read)
      if [[ "$r3" != "0" ]]; then
        echo "[HR-2] $id local_read but r3=$r3 (expected 0)" >&2
        return 1
      fi
      ;;
    xhost_read)
      # Cache hits skip forward → rh_served < trans. Lower-bound check only.
      if (( rh < HR2_R_LO || rh > HR2_R_HI )); then
        echo "[HR-2] $id xhost_read rh_served=$rh OUT OF [$HR2_R_LO, $HR2_R_HI]" >&2
        return 1
      fi
      ;;
    local_write)
      if (( lwbf > 1000 )); then
        echo "[HR-2] $id local_write but lw_blk_fwd=$lwbf (expected ~0)" >&2
        return 1
      fi
      ;;
    xhost_write)
      # Every UPDATE forwards (no read-cache shortcut). Strict ±10%.
      if (( lwbf < HR2_W_LO || lwbf > HR2_W_HI )); then
        echo "[HR-2] $id xhost_write lw_blk_fwd=$lwbf OUT OF [$HR2_W_LO, $HR2_W_HI]" >&2
        return 1
      fi
      ;;
    mix_*)
      # mix scenarios: partial forwards — relax gate to "consistent with non-zero forward"
      ;;
  esac
  return 0
}

# HR-1: online local > xhost throughput per (V,T,cache%,keydist) point.
# Keep state in $OUT_BASE/hr1_state.tsv: lines of "V T cp kd op_or_lr thpt"
# Compare current xhost_X cell against prior local_X cell's last seen rep.
hr1_check() {
  local V="$1" T="$2" cp="$3" kd="$4" sc="$5" thpt_M="$6"
  local state="$OUT_BASE/hr1_state.tsv"
  touch "$state"
  # Store current.
  echo -e "$V\t$T\t$cp\t$kd\t$sc\t$thpt_M" >> "$state"
  # If xhost cell just finished, find matching local cell same point.
  local op=""
  case "$sc" in
    xhost_read)  op="read" ;;
    xhost_write) op="write" ;;
    *) return 0 ;;
  esac
  local local_sc="local_${op}"
  # Latest thpt for the matching local cell.
  local local_thpt
  local_thpt=$(awk -v V="$V" -v T="$T" -v cp="$cp" -v kd="$kd" -v sc="$local_sc" '
    $1==V && $2==T && $3==cp && $4==kd && $5==sc { last=$6 }
    END { if(last!="") print last; else print "" }
  ' "$state")
  if [[ -z "$local_thpt" ]]; then
    # Local cell hasn't run yet at this point — can't compare. Skip.
    return 0
  fi
  # Compare.
  local ratio
  ratio=$(awk -v l="$local_thpt" -v x="$thpt_M" 'BEGIN{ if(x>0) printf "%.3f", l/x; else print "inf" }')
  if awk -v l="$local_thpt" -v x="$thpt_M" 'BEGIN{ exit !(l <= x) }'; then
    echo "[HR-1] V=$V T=$T cache%=$cp kd=$kd $op: local=$local_thpt <= xhost=$thpt_M (ratio=$ratio)" >&2
    echo "[HR-1] This violates the local-must-out-throughput-xhost invariant." >&2
    return 1
  else
    echo "  [HR-1 OK] V=$V T=$T cache%=$cp $op: local/xhost = $ratio (>1)"
  fi
  return 0
}

# Map V → build dir name (deployed under /root/FUSEE_CXL/).
build_for_V() {
  echo "build-cxl-w1-v$1"
}

# Run sequence helpers — each function plans + iterates cells for a phase.
phase0() {
  # Phase 0: 4 scenario × 2 keydist (uniform + zipf-0.99) × 4 T spot
  #          V=1024 cache_pct=10 1 rep
  local V=1024 cache=10 build
  build=$(build_for_V $V)
  local rep=1
  for T in 1 4 16 64; do
    for sc in local_read xhost_read local_write xhost_write; do
      for kd in uniform zipf-0.99; do
        local id="ph0_V${V}_T${T}_c${cache}_${sc}_${kd}"
        local th0="bench_${sc}_${kd}_h0"
        local th1="bench_${sc}_${kd}_h1"
        echo "[ph0] $id rep=$rep"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

phase1() {
  # Phase 1 V slice: V ∈ {64,256,512,1024} × T=64 × cache=10 × 4 scenario × zipf-0.99
  local T=64 cache=10 kd=zipf-0.99
  for V in 64 256 512 1024; do
    local build
    build=$(build_for_V $V)
    for sc in local_read xhost_read local_write xhost_write; do
      for rep in 1 2 3; do
        local id="ph1_V${V}_T${T}_c${cache}_${sc}_${kd}"
        local th0="bench_${sc}_${kd}_h0"
        local th1="bench_${sc}_${kd}_h1"
        echo "[ph1] $id rep=$rep"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

phase2() {
  # Phase 2 T slice: V=1024 × T ∈ {1,2,4,8,16,32,64} × cache=10 × 4 scenario × zipf-0.99
  local V=1024 cache=10 kd=zipf-0.99 build
  build=$(build_for_V $V)
  for T in 1 2 4 8 16 32 64; do
    for sc in local_read xhost_read local_write xhost_write; do
      for rep in 1 2 3; do
        local id="ph2_V${V}_T${T}_c${cache}_${sc}_${kd}"
        local th0="bench_${sc}_${kd}_h0"
        local th1="bench_${sc}_${kd}_h1"
        echo "[ph2] $id rep=$rep"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

phase3() {
  # Phase 3 cache% slice: V=1024 × T=64 × cache ∈ {1,2,5,10,20,50,100} × 4 scenario × zipf-0.99
  local V=1024 T=64 kd=zipf-0.99 build
  build=$(build_for_V $V)
  for cache in 1 2 5 10 20 50 100; do
    for sc in local_read xhost_read local_write xhost_write; do
      for rep in 1 2 3; do
        local id="ph3_V${V}_T${T}_c${cache}_${sc}_${kd}"
        local th0="bench_${sc}_${kd}_h0"
        local th1="bench_${sc}_${kd}_h1"
        echo "[ph3] $id rep=$rep"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

phase4() {
  # Phase 4 distribution slice: V=1024 × T=64 × cache=10 × 4 scenario × 4 dist
  local V=1024 T=64 cache=10 build
  build=$(build_for_V $V)
  for kd in uniform zipf-0.5 zipf-0.99 zipf-1.5; do
    for sc in local_read xhost_read local_write xhost_write; do
      for rep in 1 2 3; do
        local id="ph4_V${V}_T${T}_c${cache}_${sc}_${kd}"
        local th0="bench_${sc}_${kd}_h0"
        local th1="bench_${sc}_${kd}_h1"
        echo "[ph4] $id rep=$rep"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

phase5() {
  # Phase 5 xhost% core: V=1024 × T=64 × cache=10 × keydist=zipf-0.99
  #   xhost%=0   → local_read / local_write
  #   xhost%=50  → mix_read_50 / mix_write_50
  #   xhost%=100 → xhost_read / xhost_write
  local V=1024 T=64 cache=10 kd=zipf-0.99 build
  build=$(build_for_V $V)
  # 0% local
  for sc in local_read local_write; do
    for rep in 1 2 3; do
      local id="ph5_V${V}_T${T}_c${cache}_${sc}_${kd}"
      local th0="bench_${sc}_${kd}_h0"
      local th1="bench_${sc}_${kd}_h1"
      echo "[ph5] $id rep=$rep (xhost=0)"
      run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
               "$build" "$th0" "$th1"
      sleep 1
    done
  done
  # 50% mix
  for op in read write; do
    local sc="mix_${op}_50"
    for rep in 1 2 3; do
      local id="ph5_V${V}_T${T}_c${cache}_${sc}_zipf"
      local th0="bench_${sc}_zipf_h0"
      local th1="bench_${sc}_zipf_h1"
      echo "[ph5] $id rep=$rep (xhost=50)"
      run_cell "$id" "$V" "$T" "$cache" "$sc" "zipf" "$rep" \
               "$build" "$th0" "$th1"
      sleep 1
    done
  done
  # 100% xhost
  for sc in xhost_read xhost_write; do
    for rep in 1 2 3; do
      local id="ph5_V${V}_T${T}_c${cache}_${sc}_${kd}"
      local th0="bench_${sc}_${kd}_h0"
      local th1="bench_${sc}_${kd}_h1"
      echo "[ph5] $id rep=$rep (xhost=100)"
      run_cell "$id" "$V" "$T" "$cache" "$sc" "$kd" "$rep" \
               "$build" "$th0" "$th1"
      sleep 1
    done
  done
}

phase5_ext() {
  # Phase 5 ext: xhost% ∈ {25, 75}
  local V=1024 T=64 cache=10 build
  build=$(build_for_V $V)
  for pct in 25 75; do
    for op in read write; do
      local sc="mix_${op}_${pct}"
      for rep in 1 2 3; do
        local id="ph5ext_V${V}_T${T}_c${cache}_${sc}_zipf"
        local th0="bench_${sc}_zipf_h0"
        local th1="bench_${sc}_zipf_h1"
        echo "[ph5ext] $id rep=$rep (xhost=$pct)"
        run_cell "$id" "$V" "$T" "$cache" "$sc" "zipf" "$rep" \
                 "$build" "$th0" "$th1"
        sleep 1
      done
    done
  done
}

# Dispatch.
case "$PHASE" in
  phase0) preflight && phase0 ;;
  phase1) preflight && phase1 ;;
  phase2) preflight && phase2 ;;
  phase3) preflight && phase3 ;;
  phase4) preflight && phase4 ;;
  phase5) preflight && phase5 ;;
  phase5_ext) preflight && phase5_ext ;;
  *)
    echo "Usage: $0 <phase0|phase1|phase2|phase3|phase4|phase5|phase5_ext>" >&2
    exit 2
    ;;
esac

echo "[done] $PHASE output → $OUT_BASE"
echo "[csv]  $CSV"
