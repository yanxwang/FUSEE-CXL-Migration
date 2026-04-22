#!/usr/bin/env bash
# Full A/B/C × workload-A/C sweep on g3+g4, cross-host role mode.
#
# This is the 9 AM target of the 2026-04-21 overnight run. Each combination
# runs with 1 process per slave (num_hosts=2), i.e. one process on g3 (host 0)
# and one process on g4 (host 1), both attached to /dev/dax0.0 on the shared
# CXL memory server. Logs land under logs/g34_full_sweep_<timestamp>/.
#
# Env tunables:
#   NUM_BUCKETS   (default 65536)
#   MAX_OPS       (default 200000; per-invocation cap)
#   CACHE_MODES   (default "off on")
#   OPTS          (default "A B C")
#   WORKLOADS     (default "workloada workloadc")  <- the two we care about
#   TIMEOUT_S     (default 120)
#
# Prereq:
#   scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4
#   (the bench binaries must exist in ~/FUSEE_CXL/build-cxl/tests/)

set -u

: "${NUM_BUCKETS:=65536}"
: "${MAX_OPS:=200000}"
: "${CACHE_MODES:=off on}"
: "${OPTS:=A B C}"
: "${WORKLOADS:=workloada workloadc}"
: "${TIMEOUT_S:=120}"

: "${HOST0:=g3}"
: "${HOST1:=g4}"
: "${DEV:=/dev/dax0.0}"

stamp=$(date +%Y%m%d_%H%M%S)
OUT="$HOME/FUSEE/logs/g34_full_sweep_$stamp"
mkdir -p "$OUT"

# Preflight: slaves reachable + dax0.0 is devdax + binaries exist.
for h in "$HOST0" "$HOST1"; do
  if ! timeout 6 ssh -o BatchMode=yes -o ConnectTimeout=4 "$h" \
         'echo reachable' >/dev/null 2>&1; then
    echo "error: $h not ssh-reachable; run scripts/bootstrap_slave.sh $h first" >&2
    exit 2
  fi
  mode=$(ssh "$h" "daxctl list 2>/dev/null | python3 -c \
    \"import json,sys;d=json.load(sys.stdin);print(d[0].get('mode','?'))\"" 2>/dev/null || echo "?")
  if [[ "$mode" != "devdax" ]]; then
    echo "error: $h has /dev/dax0.0 in mode '$mode'; reconfigure to devdax first" >&2
    exit 2
  fi
  if ! ssh "$h" "test -x ~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_C"; then
    echo "error: $h missing cxl_ycsb_runner_C; run scripts/bootstrap_slave.sh $h" >&2
    exit 2
  fi
done

agg="$OUT/SUMMARY.log"
{
  echo "# FUSEE CXL g3+g4 full sweep"
  echo "# host0=$HOST0 host1=$HOST1 dev=$DEV"
  echo "# num_buckets=$NUM_BUCKETS max_ops=$MAX_OPS"
  echo "# cache_modes=$CACHE_MODES opts=$OPTS workloads=$WORKLOADS"
  echo "# started=$(date -Is)"
} > "$agg"

# Make sure the workload files are on both slaves (bootstrap only ships code).
for h in "$HOST0" "$HOST1"; do
  missing=""
  for wl in $WORKLOADS; do
    ssh "$h" "test -f ~/FUSEE_CXL/setup_workloads/${wl}.spec_load \
              && test -f ~/FUSEE_CXL/setup_workloads/${wl}.spec_trans" \
      || missing="$missing $wl"
  done
  if [[ -n "$missing" ]]; then
    echo "[$h] rsync missing workloads:$missing" | tee -a "$agg"
    ssh "$h" "mkdir -p ~/FUSEE_CXL/setup_workloads"
    for wl in $missing; do
      rsync -a "$HOME/FUSEE/setup/workloads/${wl}.spec_load" \
               "$HOME/FUSEE/setup/workloads/${wl}.spec_trans" \
               "$h:~/FUSEE_CXL/setup_workloads/"
    done
  fi
done

for wl in $WORKLOADS; do
  for opt in $OPTS; do
    bin="~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_${opt}"
    load="~/FUSEE_CXL/setup_workloads/${wl}.spec_load"
    trans="~/FUSEE_CXL/setup_workloads/${wl}.spec_trans"
    for cache in $CACHE_MODES; do
      cache_env=""; [[ "$cache" = on ]] && cache_env="FUSEE_CACHE=1"
      tag="${wl}_opt${opt}_cache${cache}"
      logdir="$OUT/$tag"
      mkdir -p "$logdir"

      # Unique per-run cookie to defeat stale-CXL-memory races in the
      # role-mode runner. Primary writes it after memset; non-primary
      # spins until it matches.
      run_cookie=$(( $(date +%s%N) ))
      cmd0="$cache_env FUSEE_RUN_COOKIE=$run_cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"
      cmd1="$cache_env FUSEE_RUN_COOKIE=$run_cookie FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 $bin $DEV $load $trans $NUM_BUCKETS $MAX_OPS"

      echo "--- RUN $tag ---" | tee -a "$agg"
      echo "cookie=$run_cookie"| tee -a "$agg"
      echo "cmd0: $cmd0"       | tee -a "$agg"
      echo "cmd1: $cmd1"       | tee -a "$agg"

      # launch host 0 (primary) FIRST so it memsets + writes cookie
      # before host 1 starts reading; host 1 waits on cookie match.
      timeout "$TIMEOUT_S" ssh "$HOST0" "$cmd0" > "$logdir/g3.log" 2>&1 &
      pid0=$!
      sleep 0.3
      timeout "$TIMEOUT_S" ssh "$HOST1" "$cmd1" > "$logdir/g4.log" 2>&1 &
      pid1=$!

      wait $pid0 $pid1 || true

      # Primary (g3, host 0) prints the YCSB summary line; pluck it out.
      ycsb_line=$(grep -m 1 "^YCSB " "$logdir/g3.log" || true)
      if [[ -n "$ycsb_line" ]]; then
        echo "$ycsb_line  # tag=$tag" | tee -a "$agg"
      else
        echo "# (no YCSB summary for $tag — inspect $logdir/g3.log and g4.log)" \
          | tee -a "$agg"
      fi
      echo | tee -a "$agg"
    done
  done
done

echo "# finished=$(date -Is)" >> "$agg"
echo "wrote $agg"
