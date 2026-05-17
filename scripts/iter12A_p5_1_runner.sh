#!/usr/bin/env bash
# iter-12A Phase 5.1 — for each of 3 residual cells, retry until we
# capture a COLLAPSE rep with the FUSEE_PROBE=1 instrumented binary.
set -u

OUTBASE="${1:?usage: $0 <outdir_base>}"
mkdir -p "$OUTBASE"

cells=("workloada 64 off 1024" "workloadb 4 on 1024" "workloadd 4 off 256")
MAX_TRY=8

for c in "${cells[@]}"; do
  set -- $c
  WL=$1; T=$2; CACHE=$3; KV=$4
  TAG="${WL}_T${T}_${CACHE}_kv${KV}"
  echo "=========================================="
  echo "[5.1] Trying cell $TAG, up to $MAX_TRY retries to capture COLLAPSE"
  echo "=========================================="
  captured=0
  for try in $(seq 1 $MAX_TRY); do
    OUT="$OUTBASE/cell_${TAG}/try${try}"
    mkdir -p "$OUT"
    bash scripts/iter12A_p5_1_probe_run.sh "$OUT" $WL $T $CACHE $KV < /dev/null 2>&1 | tail -8
    # Detect class
    if ls "$OUT/${TAG}_COLLAPSE" 2>/dev/null >/dev/null; then
      echo "[5.1] cell $TAG try $try: CAPTURED COLLAPSE"
      # Keep this dir; mark with symlink for easy access
      ln -sf "try${try}/${TAG}_COLLAPSE" "$OUTBASE/cell_${TAG}/COLLAPSE_CAPTURED"
      captured=1
      break
    elif ls "$OUT/${TAG}_TIMEOUT" 2>/dev/null >/dev/null; then
      echo "[5.1] cell $TAG try $try: TIMEOUT — treating as collapse"
      ln -sf "try${try}/${TAG}_TIMEOUT" "$OUTBASE/cell_${TAG}/COLLAPSE_CAPTURED"
      captured=1
      break
    else
      cls_dirs=$(ls -d "$OUT"/*_WIN "$OUT"/*_MID 2>/dev/null)
      echo "[5.1] cell $TAG try $try: $cls_dirs (not collapse, deleting to save disk)"
      rm -rf "$OUT"
    fi
  done
  if [ "$captured" = "0" ]; then
    echo "[5.1] cell $TAG: did NOT capture COLLAPSE in $MAX_TRY tries — skipping"
  fi
done

echo "[5.1] runner done. Collapse captures:"
ls -la "$OUTBASE"/cell_*/COLLAPSE_CAPTURED 2>/dev/null | head -10
