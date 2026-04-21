#!/usr/bin/env bash
# Re-bootstrap a PXE-ephemeral benchmark slave (g3 or g4) from the local
# persistent machine. Safe to re-run after every PXE reboot.
#
# Run this FROM /home/yanwang (the persistent orchestrator), not from the
# slave itself. The slave's homedir is on local NVMe and gets wiped on
# every PXE reboot; ~/.ssh/ is lost, so we cannot rely on the slave having
# credentials to fetch anything — we push to it via the slave's SSH that
# the orchestrator already has.
#
# Usage:
#   scripts/bootstrap_slave.sh <host> [branch]
#
# Example:
#   scripts/bootstrap_slave.sh g3
#   scripts/bootstrap_slave.sh g4 feat/cxl-migration
#
# What it does:
#   1. Refresh the bundle from our local working copy (~/FUSEE).
#   2. rsync the bundle + cxl_shm_profiling to the slave.
#   3. Clone/update ~/FUSEE_CXL on the slave from the bundle.
#   4. Install `build-essential cmake libboost-all-dev` if not present.
#   5. Configure and build the CXL-only target (libfusee_cxl + tests).
#
# After this the slave has:
#   ~/FUSEE_CXL      on the requested branch, pre-built in build-cxl/
#   ~/cxl_shm_profiling   mirrored from local
#
# Results are collected back by the caller (rsync from slave:~/FUSEE_CXL/docs/).

set -eu

host="${1:?usage: $0 <host> [branch]}"
branch="${2:-feat/cxl-migration}"

FUSEE_LOCAL="${FUSEE_LOCAL:-$HOME/FUSEE}"
CXLSHM_LOCAL="${CXLSHM_LOCAL:-$HOME/cxl_shm_profiling}"
BACKUP_DIR="${BACKUP_DIR:-$HOME/fusee_backups}"

ts=$(date +%Y%m%d_%H%M%S)
bundle="$BACKUP_DIR/fusee_bootstrap_${ts}.bundle"
mkdir -p "$BACKUP_DIR"

echo "[1/5] bundling from $FUSEE_LOCAL..."
( cd "$FUSEE_LOCAL" && git bundle create "$bundle" --all ) >/dev/null
ls -lh "$bundle"

echo "[2/5] pushing bundle + cxl_shm_profiling to $host..."
rsync -a --info=progress2 "$bundle" "$host:/tmp/fusee.bundle"
rsync -a --delete --info=progress2 "$CXLSHM_LOCAL/" "$host:~/cxl_shm_profiling/"

echo "[3/5] cloning/updating ~/FUSEE_CXL on $host..."
ssh "$host" "
  set -eu
  if [ -d \"\$HOME/FUSEE_CXL/.git\" ]; then
    cd \"\$HOME/FUSEE_CXL\"
    git fetch /tmp/fusee.bundle
    git checkout $branch 2>/dev/null || git checkout -b $branch FETCH_HEAD
    git reset --hard FETCH_HEAD
  else
    git clone -b $branch /tmp/fusee.bundle \"\$HOME/FUSEE_CXL\"
  fi
  rm -f /tmp/fusee.bundle
  cd \"\$HOME/FUSEE_CXL\"
  echo \"HEAD: \$(git log --oneline -1)\"
"

echo "[4/5] ensuring toolchain on $host..."
ssh "$host" "
  command -v cmake >/dev/null && command -v g++ >/dev/null || {
    echo 'installing build tools (sudo)...'
    sudo apt-get update -qq && sudo apt-get install -y -qq build-essential cmake
  }
  true
"

echo "[5/5] configuring + building CXL-only target on $host..."
ssh "$host" "
  set -eu
  cd \"\$HOME/FUSEE_CXL\"
  rm -rf build-cxl
  cmake -S . -B build-cxl -DCXL_ONLY=ON -DCXL_SHM_PROFILING_DIR=\"\$HOME/cxl_shm_profiling\" >/dev/null
  cmake --build build-cxl -j 2>&1 | tail -5
"

echo
echo "✓ $host bootstrapped on branch $branch"
echo "  repo:  $host:~/FUSEE_CXL"
echo "  build: $host:~/FUSEE_CXL/build-cxl"
echo "  dep:   $host:~/cxl_shm_profiling"
