#!/usr/bin/env bash
# iter-14A P5 — DRAM + CXL BW attribution: baseline STAGING vs production
# HAZARD+W1. Runs pcm-memory in background on g3 while protocol_a_ycsb
# runs on both hosts. Cell: workloada T=64 KV=1024 cache=on, 5 reps each
# build.
set -u
OUTDIR="${1:?usage: $0 <output_dir>}"
mkdir -p "$OUTDIR"

WL_DIR=/root/FUSEE_CXL/setup/workloads
DEV=/dev/dax0.0
NB=65536
OPS=200000
TIMEOUT_S=300
WL=workloada
T=64
KV=1024
CACHE=on
REPS=5
cache_flag=1

BUILDS=(build-cxl build-cxl-w1)

SUMMARY="$OUTDIR/SUMMARY.tsv"
echo -e "build\trep\ttrans_agg_thpt\tdram_read_MBps\tdram_write_MBps\tcxl_read_MBps\tcxl_write_MBps\tmem_total_MBps" > "$SUMMARY"

# CSV header parser: find indices of specific columns in line 2.
parse_csv_avg() {
  local csv="$1"
  python3 - "$csv" <<'PY'
import csv, sys
fn = sys.argv[1]
with open(fn) as f:
    rows = list(csv.reader(f))
# rows[0] = section header (,,SKT0,...), rows[1] = column names, rows[2:] = data
if len(rows) < 4:
    print("0\t0\t0\t0\t0")
    sys.exit(0)
hdr = rows[1]
data = rows[2:]
def idx(name):
    try:
        return hdr.index(name)
    except ValueError:
        return -1
i_dram_r = idx('Mem Read (MB/s)')
i_dram_w = idx('Mem Write (MB/s)')
i_mem_tot = idx('Memory (MB/s)')  # likely last column or one of the System aggregates
# CXL ports P0-P5
cxl_r = [idx(f'CXL.mem_P{p}Read') for p in range(6)]
cxl_w = [idx(f'CXL.mem_P{p}Write') for p in range(6)]
def fval(row, i):
    if i < 0 or i >= len(row): return 0.0
    try: return float(row[i])
    except: return 0.0
n = 0
sums = [0.0]*5
for r in data:
    if len(r) < 3 or not r[0] or not r[0][0].isdigit(): continue
    dram_r = fval(r, i_dram_r); dram_w = fval(r, i_dram_w); mem_tot = fval(r, i_mem_tot)
    cxl_r_sum = sum(fval(r, i) for i in cxl_r if i >= 0)
    cxl_w_sum = sum(fval(r, i) for i in cxl_w if i >= 0)
    sums[0] += dram_r; sums[1] += dram_w; sums[2] += cxl_r_sum; sums[3] += cxl_w_sum; sums[4] += mem_tot
    n += 1
if n == 0:
    print("0\t0\t0\t0\t0")
else:
    print(f"{sums[0]/n:.0f}\t{sums[1]/n:.0f}\t{sums[2]/n:.0f}\t{sums[3]/n:.0f}\t{sums[4]/n:.0f}")
PY
}

for build in "${BUILDS[@]}"; do
  BIN=/root/FUSEE_CXL/$build/tests/protocol_a_ycsb
  for rep in $(seq 1 $REPS); do
    cookie=$(date +%s%N)
    ssh -n g3 "pkill -9 -f 'protocol_a_ycsb' 2>/dev/null; pkill -9 -f 'pcm-memory' 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    ssh -n g4 "pkill -9 -f 'protocol_a_ycsb' 2>/dev/null; chmod 666 $DEV 2>/dev/null" &
    wait
    sleep 0.3
    # Start pcm-memory in background on g3 (system aggregate; representative of one CXL-attached host)
    ssh -n g3 "pcm-memory 0.5 -nc -csv=/tmp/pcm.${build}.r${rep}.csv >/tmp/pcm.log 2>&1 &
               echo \$! > /tmp/pcm.pid" >/dev/null 2>&1
    sleep 0.5  # let pcm warm up
    cmd="FUSEE_NUM_HOSTS=2 FUSEE_NUM_THREADS=$T FUSEE_RUN_COOKIE=$cookie FUSEE_REP=$rep FUSEE_CACHE=$cache_flag FUSEE_KV_SIZE=$KV FUSEE_WORKLOAD_NAME=$WL timeout $TIMEOUT_S $BIN $DEV $WL_DIR/$WL.spec_load $WL_DIR/$WL.spec_trans $NB $OPS"
    ssh -n g3 "FUSEE_HOST_ID=0 $cmd" > "$OUTDIR/${build}_rep${rep}_h0.out" 2>"$OUTDIR/${build}_rep${rep}_h0.err" &
    ssh -n g4 "FUSEE_HOST_ID=1 $cmd" > "$OUTDIR/${build}_rep${rep}_h1.out" 2>"$OUTDIR/${build}_rep${rep}_h1.err" &
    wait
    ssh -n g3 "kill \$(cat /tmp/pcm.pid) 2>/dev/null; sleep 0.5"
    scp -q g3:/tmp/pcm.${build}.r${rep}.csv "$OUTDIR/" 2>/dev/null
    line=$(grep "^YCSB" "$OUTDIR/${build}_rep${rep}_h0.out" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
      thpt=$(echo "$line" | grep -oE 'trans_agg_thpt=[0-9]+' | cut -d= -f2)
      if [ -f "$OUTDIR/pcm.${build}.r${rep}.csv" ]; then
        IFS=$'\t' read -r dram_r dram_w cxl_r cxl_w mem_tot < <(parse_csv_avg "$OUTDIR/pcm.${build}.r${rep}.csv")
      else
        dram_r=0; dram_w=0; cxl_r=0; cxl_w=0; mem_tot=0
      fi
      echo -e "$build\t$rep\t$thpt\t$dram_r\t$dram_w\t$cxl_r\t$cxl_w\t$mem_tot" >> "$SUMMARY"
      echo "[OK] $build rep=$rep thpt=$thpt dram_r=$dram_r/w=$dram_w cxl_r=$cxl_r/w=$cxl_w"
    else
      echo -e "$build\t$rep\tFAIL\t\t\t\t\t" >> "$SUMMARY"
      echo "[FAIL] $build rep=$rep"
    fi
  done
done
echo "## done. SUMMARY at $SUMMARY"
