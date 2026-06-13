#!/usr/bin/env bash
# Re-run all 3 exps with the CMake flag propagation bug fixed.
set -u
ROOT=/home/yanwang/FUSEE
LOG=$ROOT/docs/iter20A_redo_$(date +%Y%m%d_%H%M%S).log
exec > >(tee -a "$LOG") 2>&1

echo "== redo run @ $(date) =="

# Exp 2 first (we have zero decomp data, this is the gap)
echo "[chain] Exp 2 @ $(date)"
bash $ROOT/scripts/iter20A_exp2_4path_decomp.sh
echo "[chain] Exp 2 done @ $(date)"

# Exp 1 (B-H3 fix changes results — re-measure)
echo "[chain] Exp 1 @ $(date)"
bash $ROOT/scripts/iter20A_exp1_microbench_4path.sh
echo "[chain] Exp 1 done @ $(date)"

# Exp 3 (B-H3 fix changes results)
echo "[chain] Exp 3 @ $(date)"
bash $ROOT/scripts/iter20A_exp3_ycsb_n_sweep.sh
echo "[chain] Exp 3 done @ $(date)"

echo "[chain] all 3 redos done @ $(date)"
