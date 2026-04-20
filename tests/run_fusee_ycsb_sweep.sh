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
# MAX_OPS: cap each phase to this many ops (0 = unlimited). Prevents a
# 10 M-line official workload from dwarfing the sweep; override as needed.
MAX_OPS="${MAX_OPS:-0}"
# CACHE_MODES: space-separated list, each one of {off, on}. Wraps each run
# in the corresponding FUSEE_CACHE env. Default runs both so the log shows
# cache-off and cache-on side by side for every (workload, opt) pair.
CACHE_MODES="${CACHE_MODES:-off on}"

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

# Auto-detect workload pairs. Accepts two naming conventions:
#   - synthetic: <base>.load + <base>.trans (workloads_synth/)
#   - official:  <base>.spec_load + <base>.spec_trans (setup/workloads/)
# For the official set, the YCSB-supplied "workloads.spec_load" file is only
# a 9-line sample (not a real workload); we skip it by name.
if [[ -z "${WORKLOADS:-}" ]]; then
  WORKLOADS=""
  for lf in "$WL_DIR"/*.load "$WL_DIR"/*.spec_load; do
    [[ -f "$lf" ]] || continue
    base="$(basename "$lf")"
    base="${base%.spec_load}"
    base="${base%.load}"
    [[ "$base" == "workloads" ]] && continue   # skip the YCSB sample
    # dedupe in case both conventions are present
    case " $WORKLOADS " in *" $base "*) continue;; esac
    if   [[ -f "$WL_DIR/$base.trans"      ]] \
      || [[ -f "$WL_DIR/$base.spec_trans" ]]; then
      WORKLOADS="$WORKLOADS $base"
    fi
  done
fi

# Resolve the actual load/trans paths for a workload base name. Sets the
# load_path and trans_path globals. Returns non-zero if either is missing.
resolve_paths() {
  local wl="$1"
  if   [[ -f "$WL_DIR/$wl.load"      ]]; then load_path="$WL_DIR/$wl.load"
  elif [[ -f "$WL_DIR/$wl.spec_load" ]]; then load_path="$WL_DIR/$wl.spec_load"
  else return 1; fi
  if   [[ -f "$WL_DIR/$wl.trans"      ]]; then trans_path="$WL_DIR/$wl.trans"
  elif [[ -f "$WL_DIR/$wl.spec_trans" ]]; then trans_path="$WL_DIR/$wl.spec_trans"
  else return 1; fi
}

{
  echo "# FUSEE CXL YCSB sweep"
  echo "# dev=$DEV buckets=$NUM_BUCKETS wl_dir=$WL_DIR"
  echo "# opts=$OPTS workloads=$WORKLOADS"
  echo "# started=$(date -Is)"
} > "$OUT_LOG"

for wl in $WORKLOADS; do
  if ! resolve_paths "$wl"; then
    echo "# skipping $wl: missing load or trans file" >> "$OUT_LOG"
    continue
  fi
  for opt in $OPTS; do
    bin="$BUILD_DIR/tests/cxl_ycsb_runner_$opt"
    for cache in $CACHE_MODES; do
      case "$cache" in
        on)  env_prefix="FUSEE_CACHE=1"; env_token="FUSEE_CACHE=1" ;;
        off) env_prefix="";              env_token=""             ;;
        *)   echo "# unknown CACHE_MODE=$cache, skipping" >> "$OUT_LOG"; continue ;;
      esac
      header="--- workload=$wl opt=$opt cache=[$env_token] load=$(basename "$load_path") trans=$(basename "$trans_path") ---"
      echo "$header"
      echo "$header" >> "$OUT_LOG"
      if [[ -n "$env_prefix" ]]; then
        timeout "$TIMEOUT_S" env $env_prefix "$bin" \
          "$DEV" "$load_path" "$trans_path" "$NUM_BUCKETS" "$MAX_OPS" \
          >> "$OUT_LOG" 2>&1 \
          || echo "# (timeout or error workload=$wl opt=$opt cache=$cache)" >> "$OUT_LOG"
      else
        timeout "$TIMEOUT_S" "$bin" \
          "$DEV" "$load_path" "$trans_path" "$NUM_BUCKETS" "$MAX_OPS" \
          >> "$OUT_LOG" 2>&1 \
          || echo "# (timeout or error workload=$wl opt=$opt cache=$cache)" >> "$OUT_LOG"
      fi
    done
  done
done

echo "# finished=$(date -Is)" >> "$OUT_LOG"
echo "wrote $OUT_LOG"
