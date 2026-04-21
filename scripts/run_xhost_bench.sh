#!/usr/bin/env bash
# Launch an MP bench or YCSB run across g3 + g4 in role mode. The two hosts
# attach the same /dev/dax0.0 on the shared CXL memory server, coordinate
# through the shared stats region, and host 0 prints the aggregated line.
#
# Prereq: each slave already has ~/FUSEE_CXL built (run
# scripts/bootstrap_slave.sh g3 and ...g4 first).
#
# Usage:
#   run_xhost_bench.sh mp <opt> <wratio> [ops_per_host] [num_buckets] [cache]
#   run_xhost_bench.sh ycsb <opt> <workload_file_base> [num_buckets] [max_ops] [cache]
#
# Examples:
#   scripts/run_xhost_bench.sh mp C 0.5 500
#   scripts/run_xhost_bench.sh ycsb A workloadc 262144 200000 on
#
# <cache> is "off" or "on" (default off); sets FUSEE_CACHE=1 when on.
#
# Outputs:
#   logs/g34_<mp|ycsb>_opt<X>_<knob>_<ts>/{g3.log,g4.log,agg.log}
#
# Orchestrator-only; g3/g4 are PXE-wiped slaves, so nothing persistent
# lives on them. All state + logs land on local.

set -eu

kind="${1:?usage: $0 <mp|ycsb> ...}"

stamp=$(date +%Y%m%d_%H%M%S)
HOST0=g3
HOST1=g4
DEV=/dev/dax0.0

out_root="${OUT_ROOT:-$HOME/FUSEE/logs/g34_bench}"
mkdir -p "$out_root"

case "$kind" in
  mp)
    opt="${2:?opt (A|B|C)}"
    wratio="${3:?wratio}"
    ops="${4:-500}"
    buckets="${5:-8192}"
    cache="${6:-off}"
    name="mp_opt${opt}_wr${wratio}_ops${ops}_bk${buckets}_c${cache}"
    out_dir="$out_root/${name}_${stamp}"
    mkdir -p "$out_dir"
    bin="~/FUSEE_CXL/build-cxl/tests/cxl_kv_bench_mp_${opt}"
    cache_env=""; [[ "$cache" = on ]] && cache_env="FUSEE_CACHE=1"
    cmd0="$cache_env FUSEE_HOST_ID=0 $bin $DEV 2 $ops $wratio $buckets"
    cmd1="$cache_env FUSEE_HOST_ID=1 $bin $DEV 2 $ops $wratio $buckets"
    ;;
  ycsb)
    opt="${2:?opt (A|B|C)}"
    wl="${3:?workload base e.g. workloada}"
    buckets="${4:-262144}"
    max_ops="${5:-0}"
    cache="${6:-off}"
    load="/root/FUSEE_CXL/setup_workloads/${wl}.spec_load"
    trans="/root/FUSEE_CXL/setup_workloads/${wl}.spec_trans"
    # Workloads have to be on the slaves; the bootstrap script does NOT ship
    # them. Check + rsync if needed.
    for h in "$HOST0" "$HOST1"; do
      if ! ssh "$h" "test -f $load && test -f $trans" 2>/dev/null; then
        echo "[$h] workload files missing, copying setup/workloads -> ~/FUSEE_CXL/setup_workloads/"
        ssh "$h" "mkdir -p ~/FUSEE_CXL/setup_workloads"
        rsync -a "$HOME/FUSEE/setup/workloads/${wl}.spec_load" \
                 "$HOME/FUSEE/setup/workloads/${wl}.spec_trans" \
                 "$h:~/FUSEE_CXL/setup_workloads/"
      fi
    done
    name="ycsb_opt${opt}_${wl}_bk${buckets}_max${max_ops}_c${cache}"
    out_dir="$out_root/${name}_${stamp}"
    mkdir -p "$out_dir"
    bin="~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_${opt}"
    cache_env=""; [[ "$cache" = on ]] && cache_env="FUSEE_CACHE=1"
    cmd0="$cache_env FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 $bin $DEV $load $trans $buckets $max_ops"
    cmd1="$cache_env FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 $bin $DEV $load $trans $buckets $max_ops"
    ;;
  *) echo "unknown kind: $kind" >&2; exit 2 ;;
esac

echo "out_dir=$out_dir"
echo "[host1 $HOST1] $cmd1"
ssh "$HOST1" "$cmd1" > "$out_dir/g4.log" 2>&1 &
pid1=$!
sleep 0.3
echo "[host0 $HOST0] $cmd0"
ssh "$HOST0" "$cmd0" > "$out_dir/g3.log" 2>&1 &
pid0=$!

wait $pid0 $pid1 || true

{
  echo "=== $name ==="
  echo "cmd0: $cmd0"
  echo "cmd1: $cmd1"
  echo
  echo "--- g3 (host 0) ---"
  tail -15 "$out_dir/g3.log"
  echo
  echo "--- g4 (host 1) ---"
  tail -15 "$out_dir/g4.log"
} | tee "$out_dir/agg.log"

echo "logs in: $out_dir"
