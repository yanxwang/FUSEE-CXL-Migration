#!/usr/bin/env bash
# Runs Exp 1 → Exp 3 → Exp 2 in sequence. Total ~8-10 h.
set -u
ROOT=/home/yanwang/FUSEE
LOG=$ROOT/docs/iter20A_runall_$(date +%Y%m%d_%H%M%S).log
exec > >(tee -a "$LOG") 2>&1

echo "== chained run @ $(date) =="

# Exp 1 already running from caller; wait for its summary file
echo "[chain] waiting for Exp 1 summary..."
until F=$(ls $ROOT/docs/iter20A_exp1_microbench_*/summary.txt 2>/dev/null | tail -1); [ -n "$F" ] && [ -s "$F" ]; do
  sleep 60
done
echo "[chain] Exp 1 summary at $F"

echo "[chain] launching Exp 3 @ $(date)"
bash $ROOT/scripts/iter20A_exp3_ycsb_n_sweep.sh
echo "[chain] Exp 3 done @ $(date)"

echo "[chain] launching Exp 2 @ $(date)"
bash $ROOT/scripts/iter20A_exp2_4path_decomp.sh
echo "[chain] Exp 2 done @ $(date)"

echo "[chain] all 3 exps done @ $(date)"
