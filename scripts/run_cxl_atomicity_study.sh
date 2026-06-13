#!/bin/bash
# CXL cross-host write-atomicity study driver.
# See docs/study_cxl_write_atomicity/PLAN.md.
#
# Sweeps a parameter matrix; each cell launches the probe on both g1 + g2
# and harvests the host-0 SUMMARY line into raw.csv.
set -u

TS=$(date +%Y%m%d_%H%M%S)
PHASE=${PHASE:-phase1}
OUT_DIR="docs/study_cxl_write_atomicity/${PHASE}_${TS}"
mkdir -p "$OUT_DIR"

# Defaults — phase 1 baseline: aligned × memcpy × clflush_sfence × same_cl,
# sweep N only.
N_LIST=(${ATOMICITY_N_LIST:-1 2 4 8 16 32 64 128 256 512 1024})
ALIGN_LIST=(${ATOMICITY_ALIGN_LIST:-aligned})
STORE_LIST=(${ATOMICITY_STORE_LIST:-memcpy})
FENCE_LIST=(${ATOMICITY_FENCE_LIST:-clflush_sfence})
LOC_LIST=(${ATOMICITY_LOC_LIST:-same_cl})
MODE_LIST=(${ATOMICITY_MODE_LIST:-barrier})
K=${ATOMICITY_K:-1000}
PER_CELL_TIMEOUT=120

BIN=/root/FUSEE_CXL/build-cxl/tests/cxl_write_atomicity_probe

echo "== CXL write-atomicity study $(date) ==" | tee "$OUT_DIR/run.log"
echo "phase=$PHASE K=$K" | tee -a "$OUT_DIR/run.log"
echo "N: ${N_LIST[*]}" | tee -a "$OUT_DIR/run.log"
echo "align: ${ALIGN_LIST[*]}" | tee -a "$OUT_DIR/run.log"
echo "store: ${STORE_LIST[*]}" | tee -a "$OUT_DIR/run.log"
echo "fence: ${FENCE_LIST[*]}" | tee -a "$OUT_DIR/run.log"
echo "loc: ${LOC_LIST[*]}" | tee -a "$OUT_DIR/run.log"

echo "n,align,store,fence,loc,mode,K,rounds,OW_A,OW_B,OW_OTHER,INTERLEAVE,CORRUPT,wall_us,per_cl" > "$OUT_DIR/raw.csv"

CELL=0
for N in "${N_LIST[@]}"; do
  for AL in "${ALIGN_LIST[@]}"; do
    for ST in "${STORE_LIST[@]}"; do
      for FE in "${FENCE_LIST[@]}"; do
        for LO in "${LOC_LIST[@]}"; do
          for MO in "${MODE_LIST[@]}"; do
          CELL=$((CELL+1))
          CELL_TAG="n${N}_a${AL}_s${ST}_f${FE}_l${LO}_m${MO}"
          G1_LOG="$OUT_DIR/${CELL_TAG}_g1.log"
          G2_LOG="$OUT_DIR/${CELL_TAG}_g2.log"

          echo "[cell $CELL] $CELL_TAG" | tee -a "$OUT_DIR/run.log"

          RUN_ID=$(date +%s%N)
          ENV="FUSEE_RUN_COOKIE=$RUN_ID ATOMICITY_N=$N ATOMICITY_K=$K \
            ATOMICITY_ALIGN=$AL ATOMICITY_STORE=$ST ATOMICITY_FENCE=$FE \
            ATOMICITY_LOC=$LO ATOMICITY_MODE=$MO"

          ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
          ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0' &
          wait

          ssh root@g1 "$ENV FUSEE_HOST_ID=0 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g1=\$?" > "$G1_LOG" 2>&1 &
          G1_PID=$!
          sleep 1
          ssh root@g2 "$ENV FUSEE_HOST_ID=1 timeout $PER_CELL_TIMEOUT $BIN /dev/dax0.0 2>&1; echo exit_g2=\$?" > "$G2_LOG" 2>&1 &
          G2_PID=$!

          wait $G1_PID $G2_PID

          # Parse host-0 SUMMARY line.
          LINE=$(grep "^ATOMICITY " "$G1_LOG" | head -1)
          if [ -z "$LINE" ]; then
            echo "  NO SUMMARY (check $G1_LOG)" | tee -a "$OUT_DIR/run.log"
            continue
          fi
          # Strip per-cl payload first so OW_A/OW_B/etc. only match the
          # top-level fields (the per-cl segments use the same labels).
          TOP=$(echo "$LINE" | sed 's/ cl[0-9].*$//')
          PER_CL=$(echo "$LINE" | grep -oE ' cl[0-9]+=[^ ]+( cl[0-9]+=[^ ]+)*' | sed 's/^ //')
          ROUNDS=$(echo "$TOP" | grep -oP 'rounds=\K[0-9]+')
          OW_A=$(echo "$TOP" | grep -oP 'OW_A=\K[0-9]+')
          OW_B=$(echo "$TOP" | grep -oP 'OW_B=\K[0-9]+')
          OW_OTH=$(echo "$TOP" | grep -oP 'OW_OTHER=\K[0-9]+')
          INTL=$(echo "$TOP" | grep -oP 'INTERLEAVE=\K[0-9]+')
          CORR=$(echo "$TOP" | grep -oP 'CORRUPT=\K[0-9]+')
          WALL=$(echo "$TOP" | grep -oP 'wall_us=\K[0-9]+')

          echo "$N,$AL,$ST,$FE,$LO,$MO,$K,$ROUNDS,$OW_A,$OW_B,$OW_OTH,$INTL,$CORR,$WALL,\"$PER_CL\"" >> "$OUT_DIR/raw.csv"
          echo "  rounds=$ROUNDS OW_A=$OW_A OW_B=$OW_B INTL=$INTL CORR=$CORR" | tee -a "$OUT_DIR/run.log"
          done
        done
      done
    done
  done
done

echo "" | tee -a "$OUT_DIR/run.log"
echo "study done — $CELL cells: $OUT_DIR/raw.csv" | tee -a "$OUT_DIR/run.log"
