#!/usr/bin/env bash
# iter-14A P4 — production path_decomp on canonical cells.
# Smaller MAX_OPS (20k) to keep probe dump per cell ~1 GB; parse probes
# on the slave to a small summary, then scp only the summary.
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"

BIN=/root/FUSEE_CXL/build-cxl-w1-probe/tests/protocol_a_ycsb
WL_DIR=/root/FUSEE_CXL/setup/workloads
PARSER=/root/FUSEE_CXL/scripts/parse_probes_v4.py
DEV=/dev/dax0.0
NB=65536
OPS=20000
TIMEOUT_S=600
REPS=1     # 1 healthy try per cell; retry up to 5 if thpt looks anomalous

# Canonical cells per docs/iter14A_p3d_pathdecomp_spec.md §D
CELLS=(
  "workloada_T64 workloada 64 1024 on"
  "workloada_T4  workloada 4  1024 on"
  "workloadc_T64 workloadc 64 1024 on"
  "workloadb_T64 workloadb 64  256 on"
)

# Ensure parser script is on hosts (it should already be from bootstrap)
rsync -q /home/yanwang/FUSEE/scripts/parse_probes_v4.py g3:$PARSER &
rsync -q /home/yanwang/FUSEE/scripts/parse_probes_v4.py g4:$PARSER &
wait

run_cell() {
  local label=$1 wl=$2 T=$3 kv=$4 cache=$5
  local cache_flag=0
  [ "$cache" = "on" ] && cache_flag=1
  local cell_dir="$OUTDIR/$label"
  mkdir -p "$cell_dir"

  for rep in $(seq 1 $REPS); do
    local cookie=$(date +%s%N)
    local remote_dir=/tmp/iter14A_p4_${label}_rep${rep}
    ssh -n g3 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV; pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1 &
    ssh -n g4 "rm -rf $remote_dir; mkdir -p $remote_dir; chmod 666 $DEV; pkill -9 -f protocol_a_ycsb 2>/dev/null" >/dev/null 2>&1 &
    wait
    sleep 0.2
    cmd_h0="cd /root/FUSEE_CXL/build-cxl-w1-probe && FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
    cmd_h1="cd /root/FUSEE_CXL/build-cxl-w1-probe && FUSEE_PROBE_DUMP=$remote_dir/probe FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
    ssh -n g3 "$cmd_h0" > "$cell_dir/rep${rep}_h0.out" 2>"$cell_dir/rep${rep}_h0.err" &
    ssh -n g4 "$cmd_h1" > "$cell_dir/rep${rep}_h1.out" 2>"$cell_dir/rep${rep}_h1.err" &
    wait
    # Parse probes on-host, scp only parsed table
    # parse_probes_v4 rglobs `probe.*.*` from given dir; pass remote_dir (probes are at $remote_dir/probe.{pid}.{tid})
    ssh -n g3 "cd $remote_dir && python3 $PARSER . > parsed.tsv 2>parse_err.log; wc -l parsed.tsv probe.* 2>/dev/null | tail -3"
    ssh -n g4 "cd $remote_dir && python3 $PARSER . > parsed.tsv 2>parse_err.log; wc -l parsed.tsv probe.* 2>/dev/null | tail -3"
    scp -q g3:$remote_dir/parsed.tsv "$cell_dir/parsed_rep${rep}_h0.tsv" 2>/dev/null || true
    scp -q g4:$remote_dir/parsed.tsv "$cell_dir/parsed_rep${rep}_h1.tsv" 2>/dev/null || true
    ssh -n g3 "rm -rf $remote_dir" &
    ssh -n g4 "rm -rf $remote_dir" &
    wait
    line=$(grep "^YCSB" "$cell_dir/rep${rep}_h0.out" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      echo "[OK] $label rep=$rep thpt=$thpt"
    else
      echo "[FAIL] $label rep=$rep"
    fi
  done
}

for spec in "${CELLS[@]}"; do
  echo "=== $spec ==="
  run_cell $spec
done

echo "## done. dir=$OUTDIR"
