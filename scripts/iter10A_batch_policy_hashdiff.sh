#!/usr/bin/env bash
# iter-10A Phase 3 — hash-diff battery for each batch policy (C9).
# Run iter9A_redo_hash_diff_battery.sh once per policy with appropriate env.
set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"

declare -a CONFIGS=(
  "P1:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P1"
  "P2:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P2"
  "P3:FUSEE_USE_AGGREGATOR=1 FUSEE_BATCH_POLICY=P3"
)
# B0 already verified in earlier run (skipped here). Note: previous
# script invocation ran B0 first and PASSed 20/20 before hitting an
# awk bookkeeping bug (now fixed below by using $2 / $5 / $8 indices
# correctly).

# Each policy gets its own subdir + hash-diff battery run.
# Inject env into the battery via wrapper since the existing battery
# script doesn't propagate env through ssh.
overall_pass=0
overall_total=0
for cfg in "${CONFIGS[@]}"; do
  name="${cfg%%:*}"
  envs="${cfg#*:}"
  POLICY_DIR="$OUTDIR/$name"
  mkdir -p "$POLICY_DIR"
  echo "=== policy $name ==="
  # Patched script: inject envs into the cmd_h0/cmd_h1 strings at runtime.
  # Use export then execute battery.
  EXTRA_ENVS="$envs FUSEE_TLS_SIZE=1024" bash -c '
    set -u
    OUTDIR="$1"
    EXTRA_ENVS="$2"
    SUMMARY="$OUTDIR/SUMMARY.log"
    : > "$SUMMARY"
    WORKLOADS="workloada workloadb workloadc workloadd workloadf"
    KV_SIZES="8 256 512 1024"
    T=4
    OPS=10000
    NB=65536
    TIMEOUT_S=120
    DEV=/dev/dax0.0
    WL_DIR=/root/FUSEE_CXL/setup/workloads
    BIN=/root/FUSEE_CXL/build-cxl/tests/protocol_a_ycsb
    BUCKET_BYTES=$((NB * 128))
    pass=0; fail=0; total=0
    for wl in $WORKLOADS; do
      for kv in $KV_SIZES; do
        total=$((total + 1))
        cookie=$(date +%s%N)
        cell="${wl}_kv${kv}"
        H0_REMOTE=/tmp/iter10A_p3_hd_${cell}_h0.bin
        H1_REMOTE=/tmp/iter10A_p3_hd_${cell}_h1.bin
        H0_LOCAL=$OUTDIR/${cell}_h0.bin
        H1_LOCAL=$OUTDIR/${cell}_h1.bin
        ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null" &
        ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null" &
        wait
        sleep 0.2
        cmd_h0="$EXTRA_ENVS FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H0_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
        cmd_h1="$EXTRA_ENVS FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H1_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
        ssh g3 "$cmd_h0" >$OUTDIR/${cell}_h0.out 2>&1 &
        ssh g4 "$cmd_h1" >$OUTDIR/${cell}_h1.out 2>&1 &
        wait
        scp g3:$H0_REMOTE $H0_LOCAL >/dev/null 2>&1
        scp g4:$H1_REMOTE $H1_LOCAL >/dev/null 2>&1
        if [ ! -s $H0_LOCAL ] || [ ! -s $H1_LOCAL ]; then
          echo "$cell : DUMP-MISSING" | tee -a "$SUMMARY"
          fail=$((fail + 1))
          continue
        fi
        if cmp -s -n $BUCKET_BYTES $H0_LOCAL $H1_LOCAL; then
          echo "$cell : PASS" | tee -a "$SUMMARY"
          pass=$((pass + 1))
          rm -f $H0_LOCAL $H1_LOCAL
        else
          db=$(cmp -l -n $BUCKET_BYTES $H0_LOCAL $H1_LOCAL 2>/dev/null | wc -l)
          echo "$cell : FAIL ($db diverging bytes)" | tee -a "$SUMMARY"
          fail=$((fail + 1))
        fi
        ssh g3 "rm -f $H0_REMOTE" 2>/dev/null & ssh g4 "rm -f $H1_REMOTE" 2>/dev/null & wait
      done
    done
    echo "## $pass PASS / $fail FAIL of $total ##" | tee -a "$SUMMARY"
  ' bash "$POLICY_DIR" "$envs FUSEE_TLS_SIZE=1024"
  # Parse "## N PASS / M FAIL of T ##" — fields:
  #   $1=## $2=N $3=PASS $4=/ $5=M $6=FAIL $7=of $8=T $9=##
  cp_line=$(grep "^## " "$POLICY_DIR/SUMMARY.log" | tail -1)
  cp_pass=$(echo "$cp_line" | awk '{print $2}')
  cp_total=$(echo "$cp_line" | awk '{print $8}')
  : "${cp_pass:=0}"; : "${cp_total:=0}"
  overall_pass=$((overall_pass + cp_pass))
  overall_total=$((overall_total + cp_total))
done
echo
echo "## OVERALL: $overall_pass PASS / $overall_total ##"
