#!/usr/bin/env bash
# iter-12A Phase 2 driver — run R3 bisect across 5 iter-11A commits.
# Each commit measured on workload-c T=64 cache=on kv=1024 × 5 reps.
# Final SUMMARY.log captures r_avg + thpt per commit for regression detection.
set -u

OUTDIR="${1:?usage: $0 <out_dir>}"
mkdir -p "$OUTDIR"
SUM="$OUTDIR/SUMMARY.log"
echo "commit,subject,median_thpt_mops,median_r_avg_us,median_w_avg_us" > "$SUM"

# iter-11A commit timeline (oldest → newest):
COMMITS="a27dfda 455379e 5664945 3469388 e4c682b"

for c in $COMMITS; do
  subject=$(cd /home/yanwang/FUSEE && git log -1 --format=%s "$c" 2>/dev/null | tr ',' ';' | head -c 60)
  echo "[bisect-all] === commit $c — $subject ==="
  bash scripts/iter12A_p2_bisect_one_commit.sh "$OUTDIR" "$c" 2>&1 | tee "$OUTDIR/${c}.run.log"
  # Compute median r_avg + median thpt + median w_avg from per-rep csv
  per_commit_csv="$OUTDIR/${c}.csv"
  if [ ! -s "$per_commit_csv" ]; then
    echo "$c,$subject,NA,NA,NA" >> "$SUM"
    continue
  fi
  med=$(awk -F, 'NR>1 && $2!="TIMEOUT" && $2!="" {a[++n]=$2}
                 END {if(n==0){print "NA NA NA"; exit}
                      for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(a[i]>a[j]){t=a[i];a[i]=a[j];a[j]=t}
                      printf "%.4f", a[int((n+1)/2)]}' "$per_commit_csv")
  med_r=$(awk -F, 'NR>1 && $4!="NA" && $4!="" {a[++n]=$4}
                   END {if(n==0){print "NA"; exit}
                        for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(a[i]>a[j]){t=a[i];a[i]=a[j];a[j]=t}
                        printf "%.3f", a[int((n+1)/2)]}' "$per_commit_csv")
  med_w=$(awk -F, 'NR>1 && $3!="NA" && $3!="" {a[++n]=$3}
                   END {if(n==0){print "NA"; exit}
                        for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(a[i]>a[j]){t=a[i];a[i]=a[j];a[j]=t}
                        printf "%.3f", a[int((n+1)/2)]}' "$per_commit_csv")
  echo "$c,$subject,$med,$med_r,$med_w" >> "$SUM"
done

# Restore to feat/cxl-migration
cd /home/yanwang/FUSEE && git checkout feat/cxl-migration

echo "=== bisect summary ===" >&2
cat "$SUM" >&2
