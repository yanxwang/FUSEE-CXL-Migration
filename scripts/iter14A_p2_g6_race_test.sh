#!/usr/bin/env bash
# iter-14A Phase 2 G6 — cross-host R/W race test on build-cxl-p2.
# Usage: bash scripts/iter14A_p2_g6_race_test.sh <output_dir>
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"

ITERS="${ITERS:-100000}"
BIN_BASE="${BIN_BASE:-/root/FUSEE_CXL/build-cxl-p2/tests/protocol_a_rw_race_test}"
DEV="${DEV:-/dev/dax0.0}"

echo "## iter-14A P2 G6 — protocol_a_rw_race_test ITERS=$ITERS"
echo "## bin=$BIN_BASE dev=$DEV"

pass=0; fail=0
for rep in 1 2 3; do
  cookie=$(date +%s%N)
  ssh g3 "pkill -9 -f protocol_a_rw_race_test 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
  ssh g4 "pkill -9 -f protocol_a_rw_race_test 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
  wait
  sleep 0.2
  ssh g3 "FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=0 FUSEE_RUN_COOKIE=$cookie timeout 90 $BIN_BASE $ITERS $DEV" > "$OUTDIR/rep${rep}_h0.out" 2>"$OUTDIR/rep${rep}_h0.err" &
  h0=$!
  ssh g4 "FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=1 FUSEE_RUN_COOKIE=$cookie timeout 90 $BIN_BASE $ITERS $DEV" > "$OUTDIR/rep${rep}_h1.out" 2>"$OUTDIR/rep${rep}_h1.err" &
  h1=$!
  wait $h0; rc0=$?
  wait $h1; rc1=$?
  if [ $rc0 -eq 0 ] && [ $rc1 -eq 0 ] && ! grep -qiE 'FAIL|violation' "$OUTDIR"/rep${rep}_h?.out "$OUTDIR"/rep${rep}_h?.err; then
    echo "rep=$rep : PASS"
    pass=$((pass+1))
  else
    echo "rep=$rep : FAIL rc0=$rc0 rc1=$rc1"
    tail -5 "$OUTDIR"/rep${rep}_h?.err
    fail=$((fail+1))
  fi
done
echo "## $pass PASS / $fail FAIL of 3 ##"
[ "$fail" -eq 0 ] && exit 0 || exit 1
