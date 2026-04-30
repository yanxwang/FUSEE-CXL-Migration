#!/usr/bin/env bash
# iter-4A Phase 6 hash-diff battery for CxlKvStoreA_v2.
# 5 reps × T={2,4,8,16} × workload-equivalent on each host:
# both hosts run worker forks; each writes only its owned keys.
# Final bucket bytes compared via cmp.
set -u

DEV=/dev/dax0.0
NB=65536
OPS=${OPS:-10000}
BUCKET_BYTES=$((NB * 128))   # 128 B per bucket (CxlKvBucket)
DIFF_OFFSET=16

echo "## Phase 6 hash-diff battery (CxlKvStoreA_v2 owner-self) ##"
echo "## ops=$OPS num_buckets=$NB"
fail=0; pass=0
for rep in 1 2 3 4 5; do
  for T in 2 4 8 16; do
    cookie=$(date +%s%N)
    H0=/tmp/iter4A_p6_h0_r${rep}_T${T}.bin
    H1=/tmp/iter4A_p6_h1_r${rep}_T${T}.bin
    L0=$(basename $H0); L1=$(basename $H1)
    H0_CMD="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_FINAL_STATE_DUMP=$H0 /root/FUSEE_CXL/build-cxl/tests/protocol_a_v2_2host_test $DEV $NB $OPS"
    H1_CMD="FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_FINAL_STATE_DUMP=$H1 /root/FUSEE_CXL/build-cxl/tests/protocol_a_v2_2host_test $DEV $NB $OPS"

    timeout 60 ssh g3 "$H0_CMD" > /tmp/iter4A_p6_g3_r${rep}_T${T}.log 2>&1 &
    sleep 0.3
    timeout 60 ssh g4 "$H1_CMD" > /tmp/iter4A_p6_g4_r${rep}_T${T}.log 2>&1 &
    wait

    scp g3:$H0 /tmp/$L0 2>/dev/null
    scp g4:$H1 /tmp/$L1 2>/dev/null

    if [ ! -f /tmp/$L0 ] || [ ! -f /tmp/$L1 ]; then
      echo "rep=$rep T=$T : DUMP-MISSING"; fail=$((fail+1)); continue
    fi
    if cmp -s -i $DIFF_OFFSET:$DIFF_OFFSET -n $BUCKET_BYTES /tmp/$L0 /tmp/$L1; then
      echo "rep=$rep T=$T : PASS"; pass=$((pass+1))
    else
      bytes=$(cmp -l -i $DIFF_OFFSET:$DIFF_OFFSET -n $BUCKET_BYTES /tmp/$L0 /tmp/$L1 2>/dev/null | wc -l)
      echo "rep=$rep T=$T : FAIL ($bytes bytes diff)"; fail=$((fail+1))
    fi
    ssh g3 "rm -f $H0" 2>/dev/null & ssh g4 "rm -f $H1" 2>/dev/null & wait
  done
done
echo "## $pass PASS / $fail FAIL of 20 ##"
[ "$fail" -eq 0 ] && exit 0 || exit 1
