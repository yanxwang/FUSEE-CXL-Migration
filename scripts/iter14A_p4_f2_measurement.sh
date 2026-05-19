#!/usr/bin/env bash
# iter-14A F2 measurement gate: LRU sampling on cache_pool_lookup.
# Build comparison: build-cxl-w1 (baseline) vs build-cxl-w1-lru (F2).
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/MEASUREMENT.tsv"
echo -e "build\tworkload\tT\tkv\tcache\trep\ttrans_agg_thpt" > "$SUMMARY"
mkdir -p "$OUTDIR/raw"
REPS=5
BUILDS=(build-cxl-w1 build-cxl-w1-lru)
# Target cells (must improve — read-heavy):
TARGETS=(
  "workloadc 64 1024 on"
  "workloadc 64 256 on"
  "workloadb 64 1024 on"
)
# Guardrails (must not regress):
GUARDRAILS=(
  "workloada 64 1024 on"
  "workloadd 64 1024 on"
)

run_cell() {
  local build="$1" wl="$2" T="$3" kv="$4" cache="$5"
  for rep in $(seq 1 $REPS); do
    bash /home/yanwang/FUSEE/scripts/iter14A_run_cell.sh "$OUTDIR/raw" "$build" "$wl" "$T" "$kv" "$cache" 1 200000 \
      > "$OUTDIR/raw/log_${build}_${wl}_T${T}_kv${kv}_${cache}_rep${rep}.txt" 2>&1
    line=$(tail -n 5 "$OUTDIR/raw/SUMMARY.log" | grep "^YCSB" | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      echo -e "$build\t$wl\t$T\t$kv\t$cache\t$rep\t$thpt" >> "$SUMMARY"
      echo "[OK] $build $wl T=$T kv=$kv cache=$cache rep=$rep -> $thpt"
    else
      echo -e "$build\t$wl\t$T\t$kv\t$cache\t$rep\tFAIL" >> "$SUMMARY"
      echo "[FAIL] $build $wl T=$T kv=$kv cache=$cache rep=$rep"
    fi
  done
}

for build in "${BUILDS[@]}"; do
  for cell in "${TARGETS[@]}" "${GUARDRAILS[@]}"; do
    run_cell $build $cell
  done
done
echo "## done. $SUMMARY"
