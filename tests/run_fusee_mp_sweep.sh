#!/usr/bin/env bash
# Reproducible multi-process benchmark sweep for FUSEE CXL.
#
# Addresses progress-doc open-task #2: make the v3 cache-on sweep something
# you can re-run after any change, instead of copying ad-hoc invocations out
# of shell history.
#
# Produces a log in the format that docs/plot_fusee_v3_cache.py already
# understands:
#   --- cache=[...] opt=X wr=Y ---
#   HOST opt=X host=N ...
#   AGG opt=X num_hosts=N ...
#
# Usage (run on emr, after dax0.0 is in devdax mode):
#   ./run_fusee_mp_sweep.sh [build_dir] [out_log]
#
# Defaults:
#   build_dir = ../build-cxl       (where cxl_kv_bench_mp_{A,B,C} live)
#   out_log   = ../docs/fusee_mp_bench_v3.log
#
# Tunables via env:
#   DEV           (default /dev/dax0.0)
#   NUM_HOSTS     (default 4)
#   OPS_PER_HOST  (default 500)
#   NUM_BUCKETS   (default 8192)
#   WRS           (default "0.0 0.5 1.0")
#   OPTS          (default "A B C")
#   CACHE_MODES   (default "off on")   — "off" = no env var, "on" = FUSEE_CACHE=1
#   TIMEOUT_S     (default 20)

set -u

DEV="${DEV:-/dev/dax0.0}"
NUM_HOSTS="${NUM_HOSTS:-4}"
OPS_PER_HOST="${OPS_PER_HOST:-500}"
NUM_BUCKETS="${NUM_BUCKETS:-8192}"
WRS="${WRS:-0.0 0.5 1.0}"
OPTS="${OPTS:-A B C}"
CACHE_MODES="${CACHE_MODES:-off on}"
TIMEOUT_S="${TIMEOUT_S:-20}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-$script_dir/../build-cxl}"
OUT_LOG="${2:-$script_dir/../docs/fusee_mp_bench_v3.log}"

if [[ ! -x "$BUILD_DIR/tests/cxl_kv_bench_mp_C" ]]; then
  echo "error: cxl_kv_bench_mp_C not found under $BUILD_DIR/tests/." >&2
  echo "Build first: cmake --build $BUILD_DIR --target cxl_kv_bench_mp_A cxl_kv_bench_mp_B cxl_kv_bench_mp_C" >&2
  exit 2
fi

echo "# FUSEE CXL multi-proc sweep"                 >  "$OUT_LOG"
echo "# dev=$DEV hosts=$NUM_HOSTS ops=$OPS_PER_HOST buckets=$NUM_BUCKETS" >> "$OUT_LOG"
echo "# wrs=$WRS opts=$OPTS cache=$CACHE_MODES"    >> "$OUT_LOG"
echo "# started=$(date -Is)"                        >> "$OUT_LOG"

for cache in $CACHE_MODES; do
  case "$cache" in
    on)  env_prefix="FUSEE_CACHE=1"; env_token="FUSEE_CACHE=1" ;;
    off) env_prefix="";              env_token=""             ;;
    *)   echo "unknown CACHE_MODE=$cache" >&2; exit 2          ;;
  esac

  for opt in $OPTS; do
    bin="$BUILD_DIR/tests/cxl_kv_bench_mp_$opt"
    for wr in $WRS; do
      header="--- cache=[$env_token] opt=$opt wr=$wr ---"
      echo "$header"
      echo "$header" >> "$OUT_LOG"
      if [[ -n "$env_prefix" ]]; then
        timeout "$TIMEOUT_S" env $env_prefix "$bin" \
          "$DEV" "$NUM_HOSTS" "$OPS_PER_HOST" "$wr" "$NUM_BUCKETS" \
          >> "$OUT_LOG" 2>&1 || echo "# (timeout or error opt=$opt wr=$wr cache=$cache)" >> "$OUT_LOG"
      else
        timeout "$TIMEOUT_S" "$bin" \
          "$DEV" "$NUM_HOSTS" "$OPS_PER_HOST" "$wr" "$NUM_BUCKETS" \
          >> "$OUT_LOG" 2>&1 || echo "# (timeout or error opt=$opt wr=$wr cache=$cache)" >> "$OUT_LOG"
      fi
    done
  done
done

echo "# finished=$(date -Is)" >> "$OUT_LOG"
echo "wrote $OUT_LOG"

# Regenerate the cache-on plot if the plotter is present.
plotter="$script_dir/../docs/plot_fusee_v3_cache.py"
if [[ -x "$plotter" || -f "$plotter" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    python3 "$plotter" "$OUT_LOG" || echo "# (plot regen failed)"
  fi
fi
