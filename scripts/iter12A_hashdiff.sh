#!/usr/bin/env bash
# iter-12A Phase 1.6 — G1 hash-diff battery for forwarder-pool-direct
# read path. Per task plan §C13, must PASS at every (KV × workload).
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
COMMON_ENV="FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0"

pass=0; fail=0; total=0
for wl in $WORKLOADS; do
  for kv in $KV_SIZES; do
    total=$((total + 1))
    cookie=$(date +%s%N)
    cell="${wl}_kv${kv}"
    H0_REMOTE=/tmp/iter12A_hd_${cell}_h0.bin
    H1_REMOTE=/tmp/iter12A_hd_${cell}_h1.bin
    H0_LOCAL=$OUTDIR/${cell}_h0.bin
    H1_LOCAL=$OUTDIR/${cell}_h1.bin

    ssh -n g3 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh -n g4 "pkill -9 -f protocol_a_ycsb 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.2

    cmd_h0="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H0_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"
    cmd_h1="cd /root/FUSEE_CXL/build-cxl && $COMMON_ENV FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=1 FUSEE_CACHE=1 FUSEE_KV_SIZE=$kv FUSEE_WORKLOAD_NAME=$wl FUSEE_FINAL_STATE_DUMP=$H1_REMOTE timeout $TIMEOUT_S $BIN $DEV $WL_DIR/${wl}.spec_load $WL_DIR/${wl}.spec_trans $NB $OPS"

    ssh -n g3 "$cmd_h0" >$OUTDIR/${cell}_h0.out 2>$OUTDIR/${cell}_h0.err &
    ssh -n g4 "$cmd_h1" >$OUTDIR/${cell}_h1.out 2>$OUTDIR/${cell}_h1.err &
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
    ssh -n g3 "rm -f $H0_REMOTE" 2>/dev/null & ssh -n g4 "rm -f $H1_REMOTE" 2>/dev/null & wait
  done
done
echo "## $pass PASS / $fail FAIL of $total ##" | tee -a "$SUMMARY"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
