#!/bin/bash
# G3 single-CN loader/worker: loader@b2 + N workers@b2 (cid 3..2+N). No scp, no b4.
set -u
WL=$1; N=$2; OUT=$3; DIR='~/FUSEE/build/ycsb-test'
mkdir -p "$(dirname "$OUT")"
ssh b2 "cd $DIR && ./ycsb_wl_loader client_config.json $WL > /tmp/lw_loader.log 2>&1"
if ! ssh b2 "grep -q 'cached' /tmp/lw_loader.log"; then echo "LOADER FAIL ($WL N=$N)"; ssh b2 "tail -5 /tmp/lw_loader.log"; exit 1; fi
ssh b2 "cat /tmp/lw_loader.log" > "$OUT.loader.log"
cids=""; for i in $(seq 0 $((N-1))); do cids="$cids $((3+i))"; done
ssh b2 "cd $DIR && for c in $cids; do ./ycsb_wl_worker \$c client_config.json $WL >/tmp/wk_\$c.log 2>&1 </dev/null & done; wait"
agg=0; failed=0; nok=0; echo "# $WL N=$N" > "$OUT.workers.log"
for c in $cids; do
  log=$(ssh b2 "cat /tmp/wk_$c.log 2>/dev/null")
  ops=$(echo "$log"|grep -oP 'load \K[0-9]+(?= operations)'|tail -1)
  ms=$(echo "$log"|grep -oP 'time spent: \K[0-9.]+'|tail -1)
  fl=$(echo "$log"|grep -oP 'Failed \K[0-9]+'|tail -1)
  if [ -n "$ops" ] && [ -n "$ms" ]; then o=$(awk "BEGIN{printf \"%d\", $ops*1000/$ms}"); agg=$((agg+o)); failed=$((failed+${fl:-0})); nok=$((nok+1)); echo "cid$c: ops=$ops ms=$ms failed=${fl:-?} -> $o" >> "$OUT.workers.log"; else echo "cid$c: NO RESULT" >> "$OUT.workers.log"; fi
done
echo "RESULT lw $WL N=$N workers_ok=$nok/$N failed=$failed AGG=$agg ops/s ($(awk "BEGIN{printf \"%.4f\", $agg/1e6}") Mops/s)"
echo "CSV,lw,$WL,$N,$nok,$failed,$agg"
