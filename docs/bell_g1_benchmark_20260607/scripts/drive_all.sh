#!/bin/bash
# G1 full sweep driver. Order per user: micro (4-types) FIRST, then YCSB.
# Both YCSB mechanisms: paper (multi_client+split, sync getchar) and loader/worker.
# Restarts MN before every run (clean table). Appends CSV; logs progress.
#   client-count alignment:  lw N  <->  paper T=N/2  (both = N total clients)
cd "$(dirname "$0")/.."          # -> docs/bell_g1_benchmark_20260607
ROOT=$(pwd)
CSV=$ROOT/results_g1.csv
LOG=$ROOT/progress.log
REPS=${REPS:-3}
TS="1 2 4 8 14"                  # threads/node (paper, micro)  -> 2*T clients
NS="2 4 8 16 28"                 # total clients (lw)
WLS="workloada workloadb workloadc workloadd"

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
mn(){ bash "$ROOT/scripts/restart_mn.sh" >>"$LOG" 2>&1 || { log "MN RESTART FAIL"; return 1; }; }

[ -f "$CSV" ] || echo "phase,mode,workload,param,clients,extra1,extra2,extra3" > "$CSV"
log "===== G1 SWEEP START (reps=$REPS) ====="

# ---------- PHASE M2: micro throughput (paper 2-CN) ----------
log "--- micro throughput (search/insert/update/delete) ---"
for T in $TS; do for r in $(seq 1 $REPS); do
  mn || continue
  out=raw/micro_T${T}_rep${r}
  line=$(timeout 150 python3 scripts/orch.py micro _ $T "$out" 2>>"$LOG" | grep '^CSV,')
  log "micro T=$T (clients=$((2*T))) rep$r -> ${line:-FAIL}"
  echo "M2,rep$r,${line#CSV,}" >> "$CSV"
done; done

# ---------- PHASE M1: micro latency (single client) ----------
log "--- micro latency (single client) ---"
for r in $(seq 1 $REPS); do
  mn || continue
  ssh b2 "cd ~/FUSEE/build/micro-test && rm -f results/*lat* 2>/dev/null; timeout 120 ./latency_test_client client_config_2cn.json > /tmp/lat.log 2>&1; echo done"
  ssh b2 "cat /tmp/lat.log" > raw/latency_rep${r}.log 2>/dev/null
  # pull per-op latency files and compute mean/p50/p99 (values are ns)
  for op in insert search update delete; do
    f=raw/lat_${op}_rep${r}.txt
    ssh b2 "cat ~/FUSEE/build/micro-test/results/${op}_lat-*.txt 2>/dev/null" | sort -n > "$f"
    stat=$(awk '{a[NR]=$1; s+=$1} END{if(NR==0){print "NA NA NA 0"; exit} printf "%.0f %d %d %d", s/NR, a[int(NR*0.5)+1], a[int(NR*0.99)+1], NR}' "$f")
    log "latency $op rep$r (mean p50 p99 cnt): ${stat:-NA}"
    echo "M1,rep$r,latency,$op,1,$stat" >> "$CSV"
  done
done

# ---------- PHASE Y-paper: YCSB throughput (multi_client + split) ----------
log "--- YCSB paper method (multi_client + split, sync) ---"
for wl in $WLS; do for T in $TS; do for r in $(seq 1 $REPS); do
  mn || continue
  out=raw/ypaper_${wl}_T${T}_rep${r}
  line=$(timeout 150 python3 scripts/orch.py ycsb $wl $T "$out" 2>>"$LOG" | grep '^CSV,')
  log "ypaper $wl T=$T (clients=$((2*T))) rep$r -> ${line:-FAIL}"
  echo "Ypaper,rep$r,${line#CSV,}" >> "$CSV"
done; done; done

# ---------- PHASE Y-lw: YCSB throughput (loader/worker, shared trans) ----------
log "--- YCSB loader/worker method (shared trans = max contention) ---"
for wl in $WLS; do for N in $NS; do for r in $(seq 1 $REPS); do
  mn || continue
  out=raw/ylw_${wl}_N${N}_rep${r}
  line=$(timeout 200 bash scripts/lw_run.sh $wl $N "$out" 2>>"$LOG" | grep '^CSV,')
  log "ylw $wl N=$N rep$r -> ${line:-FAIL}"
  echo "Ylw,rep$r,${line#CSV,}" >> "$CSV"
done; done; done

log "===== G1 SWEEP COMPLETE ====="
