#!/usr/bin/env bash
# iter-19A Phase 3 — build the 5 flush+fence isolation variants.
# Run from workstation; ssh's to g1 + g2 and configures + builds.
set -u

HOSTS="${HOSTS:-g1 g2}"
TARGET=protocol_a_ycsb
ROOT_REMOTE=/root/FUSEE_CXL

# 5 builds: name → extra cmake flags
declare -A EXTRA=(
  ["bnf"]=""
  ["bnf-G2"]="-DFUSEE_LR_DEL_POOL_READ_FLUSH=1"
  ["bnf-G3"]="-DFUSEE_LW_DEL_RETIRE_FLUSH=1"
  ["bnf-G45"]="-DFUSEE_LW_DEL_SLOT_PUB_FLUSH=1"
  ["bnf-G6"]="-DFUSEE_LW_DEL_POOL_WRITE_FLUSH=1"
)

COMMON_FLAGS="-DCXL_ONLY=ON -DCMAKE_BUILD_TYPE=Release -DFUSEE_LR_DEL_OWNER_FLUSH=1"

build_one() {
  local H=$1 NAME=$2
  local DIR="${ROOT_REMOTE}/build-cxl-w1-v1024-${NAME}"
  local EFLAGS="${EXTRA[$NAME]}"
  echo "[${H}/${NAME}] configuring + building -> ${DIR}"
  ssh $H "mkdir -p ${DIR} && cd ${DIR} && \
    cmake ${COMMON_FLAGS} ${EFLAGS} ${ROOT_REMOTE} > cmake.log 2>&1 && \
    make -j8 ${TARGET} > make.log 2>&1 && \
    echo OK_${NAME}@${H} || \
    { echo FAIL_${NAME}@${H}; tail -30 ${DIR}/make.log; }"
}

for H in $HOSTS; do
  for NAME in bnf bnf-G2 bnf-G3 bnf-G45 bnf-G6; do
    build_one $H $NAME &
  done
done
wait
echo "all builds done"

# Verify
for H in $HOSTS; do
  echo "=== ${H} ==="
  ssh $H "for d in ${ROOT_REMOTE}/build-cxl-w1-v1024-bnf{,-G2,-G3,-G45,-G6}; do
    test -x \$d/tests/${TARGET} && echo OK: \$(basename \$d) || echo MISSING: \$(basename \$d)
  done"
done
