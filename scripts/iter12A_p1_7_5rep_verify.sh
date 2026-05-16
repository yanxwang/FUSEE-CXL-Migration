#!/usr/bin/env bash
# iter-12A Phase 1.7 — 5-rep verify on a list of (wl, T, cache, kv) cells.
# Re-uses iter12A_repro_cell.sh per cell.
#
# Input: path to a cell list file, one "wl T cache kv" per line.
# Output: SUMMARY.log "wl T cache kv rep1_thpt rep2_thpt rep3_thpt rep4_thpt rep5_thpt med_mops max_mops"
#         + bimodal classification report.
set -u

OUTDIR="${1:?usage: $0 <out_dir> <cells_file> [reps]}"
CELLS_FILE="${2:?}"
REPS="${3:-5}"
mkdir -p "$OUTDIR"
SUM="$OUTDIR/SUMMARY.log"
echo "wl T cache kv rep1 rep2 rep3 rep4 rep5 median_mops max_mops" > "$SUM"

total=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  set -- $line
  wl=$1; T=$2; cache=$3; kv=$4
  total=$((total + 1))
  cell_outdir="$OUTDIR/cell_${wl}_T${T}_${cache}_kv${kv}"
  mkdir -p "$cell_outdir"
  bash scripts/iter12A_repro_cell.sh "$cell_outdir" "$wl" "$T" "$cache" "$kv" "$REPS" > "$cell_outdir/log.txt" 2>&1 < /dev/null
  # Parse 5 rep throughputs from the summary tsv
  tsv="$cell_outdir/${wl}_T${T}_${cache}_kv${kv}_summary.tsv"
  if [ ! -s "$tsv" ]; then
    echo "$wl $T $cache $kv MISSING MISSING MISSING MISSING MISSING NA NA" >> "$SUM"
    echo "[verify] cell $wl T=$T $cache kv=$kv: no tsv" >&2
    continue
  fi
  # Get reps (5 of them), convert Mops/s to int (× 1e6)
  reps_thpt=$(awk 'NR>1 {printf("%d ", $3 * 1e6)}' "$tsv")
  # Pad if fewer than 5 reps (timeouts)
  IFS=' ' read -ra arr <<< "$reps_thpt"
  while [ ${#arr[@]} -lt 5 ]; do arr+=("0"); done
  # Compute median + max in Mops/s
  med_max=$(printf '%s\n' "${arr[@]:0:5}" | awk '
    NR<=5 {a[NR]=$1}
    END {
      for(i=1;i<=5;i++) for(j=i+1;j<=5;j++) if(a[i]>a[j]) {t=a[i];a[i]=a[j];a[j]=t;}
      printf("%.4f %.4f", a[3]/1e6, a[5]/1e6);
    }
  ')
  med=$(echo "$med_max" | awk '{print $1}')
  mx=$(echo "$med_max" | awk '{print $2}')

  cls=$(awk -v m="$mx" -v me="$med" 'BEGIN{
    if (m+0 >= 5 * me && me+0 < 0.5 && m+0 >= 1.0) print "BIMODAL"
    else if (m+0 < 0.5) print "FULL_COLLAPSE"
    else print "OK"
  }')
  echo "$wl $T $cache $kv ${arr[0]} ${arr[1]} ${arr[2]} ${arr[3]} ${arr[4]} $med $mx" >> "$SUM"
  echo "[verify] cell $wl T=$T $cache kv=$kv med=$med max=$mx -> $cls" >&2
done < "$CELLS_FILE"

# Tally
bm=$(awk 'NR>1 && $11+0 >= 5*$10 && $10+0 < 0.5 && $11+0 >= 1.0 {n++} END{print n+0}' "$SUM")
fc=$(awk 'NR>1 && $11+0 < 0.5 {n++} END{print n+0}' "$SUM")
ok=$(awk 'NR>1 && $11+0 >= 0.5 && !($11+0 >= 5*$10 && $10+0 < 0.5 && $11+0 >= 1.0) {n++} END{print n+0}' "$SUM")
echo "## total=$total BIMODAL=$bm FULL_COLLAPSE=$fc OK=$ok" | tee -a "$SUM"
echo "[verify] done. summary: $SUM" >&2
