#!/usr/bin/env bash
# iter-14A Phase 2.3 — measurement gate.
# Runs target + guardrail cells × 2 builds × 5 reps; emits SUMMARY.tsv
# for register entry + decision.
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/MEASUREMENT.tsv"
echo -e "build\tworkload\tT\tkv\tcache\trep\ttrans_agg_thpt\tw_p50\tw_p99\tr_p50\tr_p99" > "$SUMMARY"

REPS=5

# Build set: baseline (build-cxl-w1 = iter-13A production) vs P2 (build-cxl-p2)
BUILDS=(build-cxl-w1 build-cxl-p2)

# Target cells: workload-a cache=off T={4,32,64} KV=1024
TARGETS=(
  "workloada 4 1024 off"
  "workloada 32 1024 off"
  "workloada 64 1024 off"
)

# Guardrail cells (must not regress)
GUARDRAILS=(
  "workloadc 64 1024 on"
  "workloadb 64 256 on"
)

run_cell() {
  local build="$1" wl="$2" T="$3" kv="$4" cache="$5"
  for rep in $(seq 1 $REPS); do
    bash /home/yanwang/FUSEE/scripts/iter14A_run_cell.sh "$OUTDIR/raw" "$build" "$wl" "$T" "$kv" "$cache" 1 200000 \
      > "$OUTDIR/raw/log_${build}_${wl}_T${T}_kv${kv}_${cache}_rep${rep}.txt" 2>&1
    # parse trans_agg_thpt from SUMMARY.log
    line=$(tail -n 5 "$OUTDIR/raw/SUMMARY.log" | grep "^YCSB" | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      wp50=$(echo "$line" | grep -oE 'w_p50_ns=[0-9]+' | cut -d= -f2)
      wp99=$(echo "$line" | grep -oE 'w_p99_ns=[0-9]+' | cut -d= -f2)
      rp50=$(echo "$line" | grep -oE 'r_p50_ns=[0-9]+' | cut -d= -f2)
      rp99=$(echo "$line" | grep -oE 'r_p99_ns=[0-9]+' | cut -d= -f2)
      echo -e "$build\t$wl\t$T\t$kv\t$cache\t$rep\t$thpt\t$wp50\t$wp99\t$rp50\t$rp99" >> "$SUMMARY"
      echo "[OK] $build $wl T=$T kv=$kv cache=$cache rep=$rep → $thpt"
    else
      echo -e "$build\t$wl\t$T\t$kv\t$cache\t$rep\tFAIL\t\t\t\t" >> "$SUMMARY"
      echo "[FAIL] $build $wl T=$T kv=$kv cache=$cache rep=$rep"
    fi
  done
}

mkdir -p "$OUTDIR/raw"
for build in "${BUILDS[@]}"; do
  for cell in "${TARGETS[@]}" "${GUARDRAILS[@]}"; do
    run_cell $build $cell
  done
done

echo "## done. SUMMARY.tsv in $OUTDIR/MEASUREMENT.tsv"
