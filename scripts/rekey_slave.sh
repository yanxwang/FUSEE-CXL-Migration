#!/usr/bin/env bash
# Re-install local's ssh pubkey on a PXE-wiped slave via password auth.
# The password is kept in /home/yanwang/fusee_dev_credentials.md (local only,
# NOT committed). This script reads it from there at run time rather than
# baking the secret into the repo.
#
# Usage:
#   scripts/rekey_slave.sh <host>     # e.g. g3 or g4
#
# Requires: expect (for non-interactive password typing) and the
# credentials file at $CREDENTIALS_FILE (default /home/yanwang/fusee_dev_credentials.md).

set -eu

host="${1:?usage: $0 <host>}"
cred_file="${CREDENTIALS_FILE:-/home/yanwang/fusee_dev_credentials.md}"

if ! command -v expect >/dev/null 2>&1; then
  echo "error: expect not installed (apt-get install expect)" >&2
  exit 2
fi
if [[ ! -r "$cred_file" ]]; then
  echo "error: credentials file not readable: $cred_file" >&2
  exit 2
fi

# Parse the password for the host from the markdown table.
# Table rows look like:   | g3    | root    | Haidilao666  | ...
pw=$(awk -v h="$host" 'BEGIN{FS="|"} /^\|/ { gsub(/ /, "", $2); if ($2==h) { gsub(/ /, "", $4); print $4; exit } }' "$cred_file")
if [[ -z "$pw" ]]; then
  echo "error: no password entry for $host in $cred_file" >&2
  exit 2
fi

pub_key=$(cat "$HOME/.ssh/id_ed25519.pub" 2>/dev/null || cat "$HOME/.ssh/id_rsa.pub")
if [[ -z "$pub_key" ]]; then
  echo "error: no local pub key at ~/.ssh/id_ed25519.pub or id_rsa.pub" >&2
  exit 2
fi

# If passwordless ssh already works, do nothing.
if timeout 4 ssh -o BatchMode=yes -o ConnectTimeout=3 -o StrictHostKeyChecking=accept-new \
      "$host" 'exit' >/dev/null 2>&1; then
  echo "$host: passwordless ssh already works, no rekey needed"
  exit 0
fi

echo "$host: installing pubkey via password auth..."

expect <<EOF
log_user 0
set timeout 15
spawn ssh -o StrictHostKeyChecking=accept-new -o PubkeyAuthentication=no \
          -o PreferredAuthentications=password "root@$host" \
          "mkdir -p ~/.ssh && chmod 700 ~/.ssh && \
           echo '$pub_key' >> ~/.ssh/authorized_keys && \
           chmod 600 ~/.ssh/authorized_keys && echo DONE"
expect {
  -re "(?i)password:"    { send -- "$pw\r"; exp_continue }
  -re "Not allowed"      { exit 3 }
  -re "Permission denied" { exit 4 }
  "DONE"                 { exit 0 }
  timeout                { exit 5 }
  eof                    { exit 6 }
}
EOF
rc=$?

case $rc in
  0) echo "$host: rekey succeeded"; exit 0 ;;
  3) echo "$host: sshd rejected with 'Not allowed at this time' (maintenance window still open)"; exit 3 ;;
  4) echo "$host: password rejected; update $cred_file"; exit 4 ;;
  5) echo "$host: expect timed out"; exit 5 ;;
  6) echo "$host: connection closed before DONE (likely PAM gate)"; exit 6 ;;
  *) echo "$host: rekey failed (exit $rc)"; exit $rc ;;
esac
