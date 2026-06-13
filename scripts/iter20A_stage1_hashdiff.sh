#!/usr/bin/env bash
# iter-20A STAGE 1 hash-diff verification: hzres (HAZARD+RESERVED) cross-host correctness.
# Compares to bnf baseline (must both PASS byte-identical).
set -u
ROOT=/home/yanwang/FUSEE
OUTDIR=${1:-$(ls -dt $ROOT/docs/iter20A_stage1_* | head -1)}
LOG=$OUTDIR/hashdiff.log
H0=g1; H1=g2; DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=500000
V=1024; T=32
mkdir -p $OUTDIR
echo "## STAGE 1 hash-diff @ $(date)  buckets=$NUM_BUCKETS trans=$TRANS_OPS T=$T V=$V" | tee $LOG

for build in bnf hzres; do
  cookie=$(date +%s%N)
  H0_DUMP=/tmp/stage1_hd_${build}_h0.bin
  H1_DUMP=/tmp/stage1_hd_${build}_h1.bin
  L0_DUMP=$OUTDIR/${build}_hd_h0.bin
  L1_DUMP=$OUTDIR/${build}_hd_h1.bin

  for h in $H0 $H1; do
    ssh $h "pkill -9 -f protocol_a_ycsb 2>/dev/null; rm -f /tmp/stage1_hd_${build}_h*.bin" >/dev/null 2>&1
  done
  sleep 2
  builddir=/root/FUSEE_CXL/build-cxl-w1-v1024-${build}
  echo "[$build] paired run starting" | tee -a $LOG
  timeout 240 ssh $H1 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=hd FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=16384 FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada FUSEE_FINAL_STATE_DUMP=$H1_DUMP \
    ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/workloada.spec_load \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1 | tail -5" \
    > $OUTDIR/${build}_hd_h1.out 2>&1 &
  PH1=$!
  timeout 240 ssh $H0 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=hd FUSEE_CACHE=1 FUSEE_CACHE_BUCKETS=16384 FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada FUSEE_FINAL_STATE_DUMP=$H0_DUMP \
    ./tests/protocol_a_ycsb $DEV /root/FUSEE_CXL/setup/workloads/workloada.spec_load \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_trans $NUM_BUCKETS $TRANS_OPS 2>&1 | tail -5" \
    > $OUTDIR/${build}_hd_h0.out 2>&1
  wait $PH1 2>/dev/null
  scp $H0:$H0_DUMP $L0_DUMP 2>/dev/null
  scp $H1:$H1_DUMP $L1_DUMP 2>/dev/null
  if [ ! -f $L0_DUMP ] || [ ! -f $L1_DUMP ]; then
    echo "[$build] DUMP-MISSING" | tee -a $LOG; continue
  fi
  if cmp -s $L0_DUMP $L1_DUMP; then
    sz=$(stat -c %s $L0_DUMP)
    echo "[$build] PASS (byte-identical $sz bytes)" | tee -a $LOG
  else
    n=$(cmp -l $L0_DUMP $L1_DUMP 2>/dev/null | wc -l)
    off=$(cmp -l $L0_DUMP $L1_DUMP 2>/dev/null | head -1 | awk '{print $1}')
    echo "[$build] FAIL ($n bytes diff, first off=$off)" | tee -a $LOG
  fi
  ssh $H0 "rm -f $H0_DUMP" >/dev/null 2>&1
  ssh $H1 "rm -f $H1_DUMP" >/dev/null 2>&1
done
echo "## hash-diff done @ $(date)" | tee -a $LOG
