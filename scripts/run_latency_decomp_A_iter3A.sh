#!/usr/bin/env bash
# iter-3A Phase 6 — per-stage decomposition for protocol A.
#
# iter-3A FINDING: cxl_latency_decomp_A hangs when FUSEE_PER_HOST_RING=1
# is set (the SPSC ring + ack-channel cross-host exchange deadlocks for
# decomp_A's fork model). Using PER_HOST_RING=0 here gives a clean
# per-bucket vs per-slot LFM stage comparison via legacy path. K=N/A
# (K-channel routing is no-op without PER_HOST_RING).
#
# Env tunables:
#   THREADS="2 4 8 16 32"         (Phase 6 grid)
#   PER_SLOTS="0 1"               (per-slot LFM comparison)
#   REPS=2                        (per-cell reps)
#   WORKLOADS="workloada"
#   CACHE_MODES="on"
#   NUM_BUCKETS=65536
#   TIMEOUT_S=600
set -u
: "${THREADS:=2 4 8 16 32}"
: "${PER_SLOTS:=0 1}"
: "${REPS:=2}"
: "${WORKLOADS:=workloada}"
: "${CACHE_MODES:=on}"
: "${NUM_BUCKETS:=65536}"
: "${MAX_OPS:=200000}"
: "${TIMEOUT_S:=600}"
: "${HOST0:=g3}"
: "${HOST1:=g4}"
: "${DEV:=/dev/dax0.0}"

stamp=$(date +%Y%m%d_%H%M%S)
OUT="${OUT_ROOT:-$HOME/FUSEE/logs/iter3A_decomp_${stamp}}"
mkdir -p "$OUT"
agg="$OUT/SUMMARY.log"
: > "$agg"

{
  echo "# iter-3A Phase 6 decomposition on $HOST0+$HOST1 stamp=$stamp"
  echo "# threads=$THREADS k_channels=$K_CHANNELS reps=$REPS"
  echo "# workloads=$WORKLOADS cache=$CACHE_MODES num_buckets=$NUM_BUCKETS"
} | tee -a "$agg"

ok=0; fail=0
for PS in $PER_SLOTS; do
  for cache in $CACHE_MODES; do
    for wl in $WORKLOADS; do
      for T in $THREADS; do
        for rep in $(seq 1 $REPS); do
          tag="${wl}_PS${PS}_T${T}_cache${cache}_r${rep}"
          run_dir="$OUT/$tag"; mkdir -p "$run_dir"
          cookie=$(date +%s%N)
          cenv=""; [ "$cache" = on ] && cenv="FUSEE_CACHE=1 "
          # Legacy path (per_host_ring NOT set) so decomp_A doesn't hang;
          # PS toggles per-slot LFM port.
          cenv+="FUSEE_PER_SLOT_LFM_A=$PS "
          bin="/root/FUSEE_CXL/build-cxl/tests/cxl_latency_decomp_A"
          load="/root/FUSEE_CXL/setup_workloads/${wl}.spec_load"
          trans="/root/FUSEE_CXL/setup_workloads/${wl}.spec_trans"
          cmd0="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
          cmd1="${cenv}FUSEE_RUN_COOKIE=$cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
          echo "--- $tag cookie=$cookie ---" >> "$agg"
          timeout "$TIMEOUT_S" ssh "$HOST0" "$cmd0" > "$run_dir/g3.log" 2>&1 &
          p0=$!
          sleep 0.3
          timeout "$TIMEOUT_S" ssh "$HOST1" "$cmd1" > "$run_dir/g4.log" 2>&1 &
          p1=$!
          wait $p0 $p1 2>/dev/null
          line=$(grep -m1 '^DECOMP_A ' "$run_dir/g3.log" || true)
          if [ -n "$line" ]; then
            echo "$line  # $tag" | tee -a "$agg"
            ok=$((ok + 1))
          else
            echo "# FAIL $tag (see $run_dir/)" | tee -a "$agg"
            fail=$((fail + 1))
            ssh "$HOST0" "pkill -9 -f 'cxl_latency_decomp_A' 2>/dev/null" &
            ssh "$HOST1" "pkill -9 -f 'cxl_latency_decomp_A' 2>/dev/null" &
            wait
            sleep 1
          fi
        done
      done
    done
  done
done

{
  echo "# finished=$(date -Is)"
  echo "# ok=$ok fail=$fail"
} | tee -a "$agg"
echo "$OUT"
