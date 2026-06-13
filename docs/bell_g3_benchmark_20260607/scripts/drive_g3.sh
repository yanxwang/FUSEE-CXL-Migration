#!/bin/bash
# G3 full sweep (1CN+3MN, data=3, idx=3). Single node b2. Both methods.
cd "$(dirname "$0")/.."; ROOT=$(pwd); CSV=$ROOT/results_g3.csv; LOG=$ROOT/progress.log
REPS=${REPS:-3}; GRID="2 4 8 14"; WLS="workloada workloadb workloadc workloadd"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
mn(){ bash "$ROOT/scripts/restart_mn3.sh" >>"$LOG" 2>&1 || { log "MN FAIL"; return 1; }; }
[ -f "$CSV" ] || echo "phase,mode,workload,param,clients,e1,e2,e3" > "$CSV"
log "===== G3 SWEEP START (1CN+3MN data=3 idx=3, reps=$REPS) ====="
log "--- micro throughput ---"
for T in $GRID; do for r in $(seq 1 $REPS); do mn||continue
  line=$(timeout 180 python3 scripts/orch_1cn.py micro _ $T raw/micro_T${T}_rep${r} 2>>"$LOG"|grep '^CSV,')
  log "micro T=$T rep$r -> ${line:-FAIL}"; echo "M2,rep$r,${line#CSV,}" >> "$CSV"; done; done
log "--- micro latency ---"
for r in $(seq 1 $REPS); do mn||continue
  ssh b2 "cd ~/FUSEE/build/micro-test && rm -f results/*lat*; timeout 150 ./latency_test_client client_config_2cn.json >/tmp/lat.log 2>&1; echo done"
  for op in insert search update delete; do
    f=raw/lat_${op}_rep${r}.txt; ssh b2 "cat ~/FUSEE/build/micro-test/results/${op}_lat-*.txt 2>/dev/null"|sort -n > "$f"
    stat=$(awk '{a[NR]=$1;s+=$1} END{if(NR==0){print "NA NA NA 0";exit} printf "%.0f %d %d %d",s/NR,a[int(NR*.5)+1],a[int(NR*.99)+1],NR}' "$f")
    log "lat $op rep$r: ${stat}"; echo "M1,rep$r,latency,$op,1,$stat" >> "$CSV"; done; done
log "--- YCSB paper ---"
for wl in $WLS; do for T in $GRID; do for r in $(seq 1 $REPS); do mn||continue
  line=$(timeout 180 python3 scripts/orch_1cn.py ycsb $wl $T raw/ypaper_${wl}_T${T}_rep${r} 2>>"$LOG"|grep '^CSV,')
  log "ypaper $wl T=$T rep$r -> ${line:-FAIL}"; echo "Ypaper,rep$r,${line#CSV,}" >> "$CSV"; done; done; done
log "--- YCSB loader/worker ---"
for wl in $WLS; do for N in $GRID; do for r in $(seq 1 $REPS); do mn||continue
  line=$(timeout 200 bash scripts/lw_run_1cn.sh $wl $N raw/ylw_${wl}_N${N}_rep${r} 2>>"$LOG"|grep '^CSV,')
  log "ylw $wl N=$N rep$r -> ${line:-FAIL}"; echo "Ylw,rep$r,${line#CSV,}" >> "$CSV"; done; done; done
log "===== G3 COMPLETE ====="
