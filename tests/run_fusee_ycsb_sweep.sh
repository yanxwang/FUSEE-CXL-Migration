#!/usr/bin/env bash
# Single-host YCSB workload sweep for the CXL-FUSEE KV store.
#
# Addresses two progress-doc gaps:
#   - Phase 5 was marked done but had no per-workload perf log committed.
#   - Open-task #5: workflow for running the official YCSB workloads once
#     setup/download_workload.sh has populated ~/FUSEE/workloads/.
#
# The runner binaries (cxl_ycsb_runner_{A,B,C}) already accept both the
# synthetic spec format (workloads_synth/wl_*.{load,trans}) and the real
# YCSB spec format ("INSERT usertable userN" / "READ usertable userN"),
# so this script works against either.
#
# Usage (run on emr, after dax0.0 is in devdax mode):
#   ./run_fusee_ycsb_sweep.sh [build_dir] [out_log]
#
# Env tunables:
#   DEV            (default /dev/dax0.0)
#   NUM_BUCKETS    (default 16384)
#   WL_DIR         (default ~/FUSEE/workloads_synth; try workloads/ for real)
#   WORKLOADS      (default auto-detected from WL_DIR)
#   OPTS           (default "A B C")
#   TIMEOUT_S      (default 60)

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEV="${DEV:-/dev/dax0.0}"
NUM_BUCKETS="${NUM_BUCKETS:-16384}"
WL_DIR="${WL_DIR:-$script_dir/../workloads_synth}"
OPTS="${OPTS:-A B C}"
TIMEOUT_S="${TIMEOUT_S:-60}"

BUILD_DIR="${1:-$script_dir/../build-cxl}"
OUT_LOG="${2:-$script_dir/../docs/fusee_ycsb_sweep.log}"

if [[ ! -d "$WL_DIR" ]]; then
  echo "error: workload dir $WL_DIR not found" >&2
  exit 2
fi
if [[ ! -x "$BUILD_DIR/tests/cxl_ycsb_runner_C" ]]; then
  echo "error: cxl_ycsb_runner_C missing under $BUILD_DIR/tests/." >&2
  echo "Build first: cmake --build $BUILD_DIR --target cxl_ycsb_runner_A cxl_ycsb_runner_B cxl_ycsb_runner_C" >&2
  exit 2
fi

# Auto-detect workload triples: any file matching *.load whose .trans also exists.
if [[ -z "${WORKLOADS:-}" ]]; then
  WORKLOADS=""
  for lf in "$WL_DIR"/*.load; do
    [[ -f "$lf" ]] || continue
    base="$(basename "${lf%.load}")"
    trans="$WL_DIR/$base.trans"
    [[ -f "$trans" ]] || continue
    WORKLOADS="$WORKLOADS $base"
  done
fi

{
  echo "# FUSEE CXL YCSB sweep"
  echo "# dev=$DEV buckets=$NUM_BUCKETS wl_dir=$WL_DIR"
  echo "# opts=$OPTS workloads=$WORKLOADS"
  echo "# started=$(date -Is)"
} > "$OUT_LOG"

for wl in $WORKLOADS; do
  load="$WL_DIR/$wl.load"
  trans="$WL_DIR/$wl.trans"
  for opt in $OPTS; do
    bin="$BUILD_DIR/tests/cxl_ycsb_runner_$opt"
    header="--- workload=$wl opt=$opt ---"
    echo "$header"
    echo "$header" >> "$OUT_LOG"
    timeout "$TIMEOUT_S" "$bin" "$DEV" "$load" "$trans" "$NUM_BUCKETS" \
      >> "$OUT_LOG" 2>&1 \
      || echo "# (timeout or error workload=$wl opt=$opt)" >> "$OUT_LOG"
  done
done

echo "# finished=$(date -Is)" >> "$OUT_LOG"
echo "wrote $OUT_LOG"
