#!/usr/bin/env bash
# iter-9A redo Phase 1 + Phase 2 — C5 hash-diff battery for Protocol A.
#
# Per task_plan_iter9A.md §C5: G1 hash-diff runs after Phase 1
# (varlen wired) and again after Phase 2 (3-ring split + ForwardStaging
# arena), each must PASS at every (KV size × workload) cell.
#
# Mechanism:
#   1. Both hosts launch protocol_a_ycsb with FUSEE_FINAL_STATE_DUMP
#      pointing to a per-host bucket-array dump file.
#   2. After protocol_a_ycsb's cross-host barrier 3 syncs both hosts,
#      each host primary client flushes the bucket array from CXL and
#      dumps raw bucket bytes to its file.
#   3. Pull both files back, byte-cmp them.
#   4. Under §I9 strict-A linearizability they must be byte-identical.
#
# Usage:
#   scripts/iter9A_redo_hash_diff_battery.sh <output_dir>
#
# Exit 0 if every cell PASSes. Exit 1 if any cell FAILs (per CLAUDE.md
# C5: violation = iter not complete).

set -u

OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"
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
    H0_REMOTE=/tmp/iter9A_redo_hd_${cell}_h0.bin
    H1_REMOTE=/tmp/iter9A_redo_hd_${cell}_h1.bin
    H0_LOCAL=$OUTDIR/${cell}_h0.bin
    H1_LOCAL=$OUTDIR/${cell}_h1.bin

    ssh g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null" &
    ssh g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 /dev/dax0.0 2>/dev/null" &
    wait
    sleep 0.2

    cmd_h0="cd /root/FUSEE_CXL/build-cxl && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H0_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
    cmd_h1="cd /root/FUSEE_CXL/build-cxl && FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H1_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"

    ssh g3 "$cmd_h0" >$OUTDIR/${cell}_h0.out 2>$OUTDIR/${cell}_h0.err &
    h0p=$!
    ssh g4 "$cmd_h1" >$OUTDIR/${cell}_h1.out 2>$OUTDIR/${cell}_h1.err &
    h1p=$!
    wait $h0p; rc0=$?
    wait $h1p; rc1=$?

    if [ $rc0 -ne 0 ] || [ $rc1 -ne 0 ]; then
      echo "$cell : RUN-FAIL rc0=$rc0 rc1=$rc1" | tee -a "$SUMMARY"
      fail=$((fail + 1))
      continue
    fi

    scp g3:$H0_REMOTE $H0_LOCAL >/dev/null 2>&1
    scp g4:$H1_REMOTE $H1_LOCAL >/dev/null 2>&1
    if [ ! -s $H0_LOCAL ] || [ ! -s $H1_LOCAL ]; then
      echo "$cell : DUMP-MISSING (h0=$(stat -c%s $H0_LOCAL 2>/dev/null || echo NA) h1=$(stat -c%s $H1_LOCAL 2>/dev/null || echo NA))" | tee -a "$SUMMARY"
      fail=$((fail + 1))
      ssh g3 "rm -f $H0_REMOTE" 2>/dev/null & ssh g4 "rm -f $H1_REMOTE" 2>/dev/null & wait
      continue
    fi

    if cmp -s -n $BUCKET_BYTES $H0_LOCAL $H1_LOCAL; then
      echo "$cell : PASS" | tee -a "$SUMMARY"
      pass=$((pass + 1))
      rm -f $H0_LOCAL $H1_LOCAL  # cleanup on PASS
    else
      diff_bytes=$(cmp -l -n $BUCKET_BYTES $H0_LOCAL $H1_LOCAL 2>/dev/null | wc -l)
      echo "$cell : FAIL ($diff_bytes diverging bytes / $BUCKET_BYTES)" | tee -a "$SUMMARY"
      fail=$((fail + 1))
    fi

    ssh g3 "rm -f $H0_REMOTE" 2>/dev/null & ssh g4 "rm -f $H1_REMOTE" 2>/dev/null & wait
  done
done

echo "## $pass PASS / $fail FAIL of $total ##" | tee -a "$SUMMARY"
[ "$fail" -eq 0 ] && exit 0 || exit 1
