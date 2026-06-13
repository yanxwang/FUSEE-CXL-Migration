#!/usr/bin/env bash
# iter-19A Phase 3 hash-diff verification.
# For each isolation build (G2, G3, G45, G6) run 1 paired workload-a cell + dump,
# then cmp cross-host buckets. PASS = both hosts byte-identical (per §I9 strict-A).
# FAIL = removed flush+fence broke cross-host visibility.
set -u
ROOT=/home/yanwang/FUSEE
OUTDIR=${1:-$(ls -dt $ROOT/docs/iter19A_phase3_flush_iso_* | head -1)}
LOG=$OUTDIR/hashdiff.log
H0=g1; H1=g2
DEV=/dev/dax0.0
NUM_BUCKETS=8388608
TRANS_OPS=500000
V=1024
T=32

# Bucket section bytes = num_buckets * sizeof(CxlKvBucket=128B)
BUCKET_BYTES=$((NUM_BUCKETS * 128))

mkdir -p $OUTDIR
echo "## Phase 3 hash-diff verification" | tee $LOG
echo "@ $(date)  -- num_buckets=$NUM_BUCKETS  trans_ops=$TRANS_OPS  T=$T  V=$V" | tee -a $LOG
echo "" | tee -a $LOG

for build in bnf bnf-G2 bnf-G3 bnf-G45 bnf-G6; do
  cookie=$(date +%s%N)
  H0_DUMP=/tmp/phase3_hd_${build}_h0.bin
  H1_DUMP=/tmp/phase3_hd_${build}_h1.bin
  L0_DUMP=$OUTDIR/$(basename $H0_DUMP)
  L1_DUMP=$OUTDIR/$(basename $H1_DUMP)

  for h in $H0 $H1; do
    ssh $h "pgrep -x protocol_a_ycsb 2>/dev/null | xargs -r kill -9; rm -f /tmp/phase3_hd_${build}_h*.bin" >/dev/null 2>&1
  done
  sleep 1

  builddir=/root/FUSEE_CXL/build-cxl-w1-v1024-${build}
  echo "[$build] starting paired run ..." | tee -a $LOG
  timeout 240 ssh $H1 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=hd FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=16384 FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada \
    FUSEE_FINAL_STATE_DUMP=$H1_DUMP \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_load \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1 | tail -10" > $OUTDIR/${build}_hd_h1.out 2>&1 &
  PH1=$!

  timeout 240 ssh $H0 "cd $builddir && \
    FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T \
    FUSEE_RUN_COOKIE=$cookie FUSEE_REP=hd FUSEE_CACHE=1 \
    FUSEE_CACHE_BUCKETS=16384 FUSEE_KV_SIZE=$V \
    FUSEE_WORKLOAD_NAME=workloada \
    FUSEE_FINAL_STATE_DUMP=$H0_DUMP \
    ./tests/protocol_a_ycsb $DEV \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_load \
      /root/FUSEE_CXL/setup/workloads/workloada.spec_trans \
      $NUM_BUCKETS $TRANS_OPS 2>&1 | tail -10" > $OUTDIR/${build}_hd_h0.out 2>&1
  wait $PH1 2>/dev/null

  # Pull dumps
  scp $H0:$H0_DUMP $L0_DUMP 2>/dev/null
  scp $H1:$H1_DUMP $L1_DUMP 2>/dev/null
  if [ ! -f $L0_DUMP ] || [ ! -f $L1_DUMP ]; then
    echo "[$build] DUMP-MISSING (h0=$(test -f $L0_DUMP && echo Y || echo N) h1=$(test -f $L1_DUMP && echo Y || echo N))" | tee -a $LOG
    continue
  fi
  sz0=$(stat -c %s $L0_DUMP); sz1=$(stat -c %s $L1_DUMP)
  if [ "$sz0" != "$sz1" ]; then
    echo "[$build] SIZE-MISMATCH h0=$sz0 h1=$sz1" | tee -a $LOG
    continue
  fi
  if cmp -s $L0_DUMP $L1_DUMP; then
    echo "[$build] PASS (byte-identical, $sz0 bytes)" | tee -a $LOG
  else
    n_diff=$(cmp -l $L0_DUMP $L1_DUMP 2>/dev/null | wc -l)
    first_off=$(cmp -l $L0_DUMP $L1_DUMP 2>/dev/null | head -1 | awk '{print $1}')
    echo "[$build] FAIL ($n_diff bytes diff, first @ off=$first_off)" | tee -a $LOG
  fi
  # Cleanup
  ssh $H0 "rm -f $H0_DUMP" >/dev/null 2>&1 &
  ssh $H1 "rm -f $H1_DUMP" >/dev/null 2>&1 &
  wait
done

echo "" | tee -a $LOG
echo "## hash-diff done @ $(date)" | tee -a $LOG
