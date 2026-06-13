#!/bin/bash
# Loader/worker single run: one (workload, N) cell.
# loader@b2 loads table + writes cache.dump; scp to b4; launch N workers
# (N/2 per node, all run the SAME spec_trans = shared Zipf = max contention),
# each ~100k ops; per-worker ops/s = ops*1000/time_spent_ms; aggregate = sum.
# Usage: lw_run.sh <workload> <N> <outprefix>
set -u
WL=$1; N=$2; OUT=$3
N2=$((N/2))
DIR='~/FUSEE/build/ycsb-test'
mkdir -p "$(dirname "$OUT")"

# 1. loader on b2 (runs to completion: load + dump_cache)
ssh b2 "cd $DIR && ./ycsb_wl_loader client_config.json $WL > /tmp/lw_loader.log 2>&1"
if ! ssh b2 "grep -q 'cached' /tmp/lw_loader.log"; then
  echo "LOADER FAIL ($WL N=$N)"; ssh b2 "tail -5 /tmp/lw_loader.log"; exit 1
fi
ssh b2 "cat /tmp/lw_loader.log" > "$OUT.loader.log"

# 2. ship cache.dump to b4
ssh b2 "cd $DIR && cp cache.dump /tmp/cache.dump"
scp -q b2:/tmp/cache.dump /tmp/cache.dump.$$ && scp -q /tmp/cache.dump.$$ b4:/tmp/cache.dump && \
  ssh b4 "cp /tmp/cache.dump $DIR/cache.dump" && rm -f /tmp/cache.dump.$$

# 3. launch N workers concurrently (b2 cid 3.., b4 cid 17..)
cids_b2=""; cids_b4=""
for i in $(seq 0 $((N2-1))); do cids_b2="$cids_b2 $((3+i))"; cids_b4="$cids_b4 $((17+i))"; done
# workers have no getchar; run to completion. `wait` inside ssh blocks until done.
ssh b2 "cd $DIR && for c in $cids_b2; do ./ycsb_wl_worker \$c client_config.json $WL >/tmp/wk_\$c.log 2>&1 </dev/null & done; wait" &
ssh b4 "cd $DIR && for c in $cids_b4; do ./ycsb_wl_worker \$c client_config_worker.json $WL >/tmp/wk_\$c.log 2>&1 </dev/null & done; wait" &
wait

# 4. parse: per-worker ops/s = load_ops*1000/time_spent_ms ; sum + failed
agg=0; failed_tot=0; nok=0
echo "# $WL N=$N per-worker" > "$OUT.workers.log"
for c in $cids_b2; do
  log=$(ssh b2 "cat /tmp/wk_$c.log 2>/dev/null")
  ops=$(echo "$log" | grep -oP 'load \K[0-9]+(?= operations)' | tail -1)
  ms=$(echo "$log"  | grep -oP 'time spent: \K[0-9.]+' | tail -1)
  fl=$(echo "$log"  | grep -oP 'Failed \K[0-9]+' | tail -1)
  if [ -n "$ops" ] && [ -n "$ms" ]; then
    o=$(awk "BEGIN{printf \"%d\", $ops*1000/$ms}")
    agg=$((agg+o)); failed_tot=$((failed_tot+${fl:-0})); nok=$((nok+1))
    echo "b2 cid$c: ops=$ops ms=$ms failed=${fl:-?} -> $o ops/s" >> "$OUT.workers.log"
  else echo "b2 cid$c: NO RESULT" >> "$OUT.workers.log"; fi
done
for c in $cids_b4; do
  log=$(ssh b4 "cat /tmp/wk_$c.log 2>/dev/null")
  ops=$(echo "$log" | grep -oP 'load \K[0-9]+(?= operations)' | tail -1)
  ms=$(echo "$log"  | grep -oP 'time spent: \K[0-9.]+' | tail -1)
  fl=$(echo "$log"  | grep -oP 'Failed \K[0-9]+' | tail -1)
  if [ -n "$ops" ] && [ -n "$ms" ]; then
    o=$(awk "BEGIN{printf \"%d\", $ops*1000/$ms}")
    agg=$((agg+o)); failed_tot=$((failed_tot+${fl:-0})); nok=$((nok+1))
    echo "b4 cid$c: ops=$ops ms=$ms failed=${fl:-?} -> $o ops/s" >> "$OUT.workers.log"
  else echo "b4 cid$c: NO RESULT" >> "$OUT.workers.log"; fi
done
echo "RESULT lw $WL N=$N workers_ok=$nok/$N failed=$failed_tot AGG=$agg ops/s ($(awk "BEGIN{printf \"%.4f\", $agg/1e6}") Mops/s)"
echo "CSV,lw,$WL,$N,$nok,$failed_tot,$agg"
