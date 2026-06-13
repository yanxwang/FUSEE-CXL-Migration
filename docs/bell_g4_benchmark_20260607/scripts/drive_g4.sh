#!/bin/bash
# G4 full sweep (4CN+4MN co-located, scheme①: MN CPU node1 + interleave mem; CN NUMA0).
# ypaper only (ylw non-functional at idx>=3, evidenced by G3 38/48 FAIL).
cd "$(dirname "$0")/.."; ROOT=$(pwd); CSV=$ROOT/results_g4.csv; LOG=$ROOT/progress.log
REPS=${REPS:-3}; GRID="1 2 4 7"; WLS="workloada workloadb workloadc workloadd"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
mn(){ bash "$ROOT/scripts/restart_mn4.sh" >>"$LOG" 2>&1 || { log "MN FAIL"; return 1; }; }
[ -f "$CSV" ] || echo "phase,mode,workload,Tpernode,clients,e1,e2" > "$CSV"
log "===== G4 SWEEP START (4CN+4MN co-loc, data=idx=4, scheme①, reps=$REPS) ====="
log "--- micro throughput (4CN) ---"
for T in $GRID; do for r in $(seq 1 $REPS); do mn||continue
  line=$(timeout 200 python3 scripts/orch_g4.py micro _ $T raw/micro_T${T}_rep${r} 2>>"$LOG"|grep '^CSV,')
  log "micro T=$T (cl=$((4*T))) rep$r -> ${line:-FAIL}"; echo "M2,rep$r,${line#CSV,}" >> "$CSV"; done; done
log "--- micro latency (single CN, b1 NUMA0) ---"
for r in $(seq 1 $REPS); do mn||continue
  ssh b1 "cd ~/FUSEE/build/micro-test && rm -f results/*lat*; timeout 150 numactl --cpunodebind=0 --membind=0 ./latency_test_client client_config_2cn.json >/tmp/lat.log 2>&1; echo done"
  for op in insert search update delete; do
    f=raw/lat_${op}_rep${r}.txt; ssh b1 "cat ~/FUSEE/build/micro-test/results/${op}_lat-*.txt 2>/dev/null"|sort -n > "$f"
    stat=$(awk '{a[NR]=$1;s+=$1} END{if(NR==0){print "NA NA NA 0";exit} printf "%.0f %d %d %d",s/NR,a[int(NR*.5)+1],a[int(NR*.99)+1],NR}' "$f")
    log "lat $op rep$r: ${stat}"; echo "M1,rep$r,latency,$op,1,$stat" >> "$CSV"; done; done
log "--- YCSB paper (4CN) ---"
for wl in $WLS; do for T in $GRID; do for r in $(seq 1 $REPS); do mn||continue
  line=$(timeout 220 python3 scripts/orch_g4.py ycsb $wl $T raw/ypaper_${wl}_T${T}_rep${r} 2>>"$LOG"|grep '^CSV,')
  log "ypaper $wl T=$T (cl=$((4*T))) rep$r -> ${line:-FAIL}"; echo "Ypaper,rep$r,${line#CSV,}" >> "$CSV"; done; done; done
log "===== G4 COMPLETE (ylw skipped: non-functional at idx=4) ====="
