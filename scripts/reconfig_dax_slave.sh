#!/usr/bin/env bash
# Reconfigure a slave's CXL dax device to devdax mode (it boots in
# system-ram by default on PXE rootfs) and chmod 666 it so non-root
# benchmarks can mmap it.
#
# Default platform (2026-05-29+): g1 / g2.
# Legacy / iter repro: g3 / g4.
#
# Default device: dax0.0 (this is the CXL device that is SHARED across
# the slave pair on g1/g2 and g3/g4, the one all FUSEE benchmarks attach
# to).
#
# Hardware note (2026-05-29):
#   - g1 has TWO dax devices: dax0.0 (256 GiB, shared with g2 via
#     CXL switch) AND dax1.0 (512 GiB, **private direct-attached CXL
#     module** — NOT shared with g2). Do NOT reconfigure dax1.0 here
#     unless explicitly asked; use `--dev dax1.0` to override.
#   - g2 has only dax0.0 (256 GiB, shared).
#   - g3/g4 each have dax0.0 (512 GiB, shared).
#
# Usage:
#   scripts/reconfig_dax_slave.sh <host> [--dev <dax>]
#
# Examples:
#   scripts/reconfig_dax_slave.sh g1                  # dax0.0 only (default)
#   scripts/reconfig_dax_slave.sh g2
#   scripts/reconfig_dax_slave.sh g1 --dev dax1.0     # private CXL on g1 only
#
# Exits 0 on success, non-zero on any step failure.

set -eu

host=""
dev="dax0.0"

while [ $# -gt 0 ]; do
  case "$1" in
    --dev) dev="$2"; shift 2 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      if [ -z "$host" ]; then host="$1"; shift
      else echo "error: unexpected arg: $1" >&2; exit 2
      fi
      ;;
  esac
done

if [ -z "$host" ]; then
  echo "usage: $0 <host> [--dev dax0.0]" >&2
  exit 2
fi

echo "[reconfig] $host: ${dev} → devdax mode"
ssh "$host" "
  set -e
  daxctl reconfigure-device --mode=devdax --force ${dev}
  chmod 666 /dev/${dev}
  echo '[reconfig] $host final state:'
  daxctl list -d ${dev}
  ls -la /dev/${dev}
"
echo "[reconfig] $host: ${dev} OK"
