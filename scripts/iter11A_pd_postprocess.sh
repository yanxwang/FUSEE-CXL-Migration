#!/usr/bin/env bash
# iter-11A Phase 5 post-processor: parses captured probes via
# parse_probes_v4.py and writes per-cell per_stage_decomp.md.
#
# parse_probes_v4.py uses rglob, so pointing it at the parent
# probes_healthy/ (which contains g3_try* and g4_try* subdirs) picks
# up BOTH hosts' probe.* files in a single pass — no copies needed,
# which is critical when raw probes total tens of GiB per cell.
#
# Usage:
#   scripts/iter11A_pd_postprocess.sh <pd_dir> <cells_file>
set -u

PD_DIR="${1:?usage: $0 <pd_dir> <cells.txt>}"
CELLS="${2:?usage: $0 <pd_dir> <cells.txt>}"
PARSER=/home/yanwang/FUSEE/scripts/parse_probes_v4.py

while IFS= read -r line; do
  [ -z "$line" ] && continue
  [[ "$line" =~ ^# ]] && continue
  read -r LABEL WL_NAME KV T CACHE_MODE EXPECTED <<< "$line"
  CELL_DIR="$PD_DIR/per_cell/$LABEL"
  [ ! -d "$CELL_DIR" ] && { echo "skip $LABEL (no cell dir)"; continue; }

  HEALTHY_MD=""
  ANOM_MD=""
  if compgen -G "$CELL_DIR/probes_healthy/g3_try*/probe.*" > /dev/null; then
    HEALTHY_MD=$(python3 "$PARSER" "$CELL_DIR/probes_healthy" 2>/dev/null)
    echo "$HEALTHY_MD" > "$CELL_DIR/healthy_stages.tsv"
  fi
  if compgen -G "$CELL_DIR/probes_anomaly/g3_try*/probe.*" > /dev/null; then
    ANOM_MD=$(python3 "$PARSER" "$CELL_DIR/probes_anomaly" 2>/dev/null)
    echo "$ANOM_MD" > "$CELL_DIR/anomaly_stages.tsv"
  fi

  {
    echo "# $LABEL per-stage decomposition"
    echo
    echo "**Cell**: workload=$WL_NAME kv=$KV T=$T cache=$CACHE_MODE"
    echo "**iter-10A expected**: ~$EXPECTED Mops/s"
    echo "**Build**: TLS=1024 + lock-free CAS cache_pool + B0 + iter-11A Phase 0-4 (FUSEE_PROBE=1)"
    echo
    echo "## Healthy capture"
    echo
    if [ -n "$HEALTHY_MD" ]; then echo "$HEALTHY_MD"; else echo "_(no healthy probes captured)_"; fi
    echo
    echo "## Anomaly capture"
    echo
    if [ -n "$ANOM_MD" ]; then echo "$ANOM_MD"; else echo "_(no anomaly observed in 12 tries)_"; fi
  } > "$CELL_DIR/per_stage_decomp.md"

  echo "[pd-post] $LABEL: healthy=$([ -n "$HEALTHY_MD" ] && echo Y || echo n) anomaly=$([ -n "$ANOM_MD" ] && echo Y || echo n)"
done < "$CELLS"
