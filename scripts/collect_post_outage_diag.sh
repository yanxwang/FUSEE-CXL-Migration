#!/usr/bin/env bash
# Collect diagnostic info from a slave that just came back from an outage.
# Pulls journalctl for the last boot, cron state, PAM access/time config,
# uptime, last reboot list, and kernel messages.
#
# Usage:
#   scripts/collect_post_outage_diag.sh <host> [out_dir]
#
# Out_dir defaults to logs/g34_diag_<host>_<timestamp>/.

set -eu

host="${1:?usage: $0 <host> [out_dir]}"
stamp=$(date +%Y%m%d_%H%M%S)
out="${2:-$HOME/FUSEE/logs/g34_diag_${host}_${stamp}}"
mkdir -p "$out"

if ! timeout 6 ssh -o BatchMode=yes -o ConnectTimeout=4 "$host" \
       'echo reachable' >/dev/null 2>&1; then
  echo "error: $host not ssh-reachable" >&2
  exit 2
fi

echo "[1/8] uptime + last reboot..."
ssh "$host" 'uptime; echo ---last---; last -n 10 2>&1 | head -12; \
             echo ---who---; who' > "$out/uptime.txt" 2>&1

echo "[2/8] journalctl last boot (since 02:00 CDT)..."
ssh "$host" 'journalctl -b --since "02:00" --no-pager 2>&1 | head -500' \
  > "$out/journalctl_last_boot.txt" 2>&1

echo "[3/8] journalctl previous boot (outage window)..."
ssh "$host" 'journalctl -b -1 --no-pager 2>&1 | tail -300' \
  > "$out/journalctl_prev_boot.txt" 2>&1

echo "[4/8] dmesg tail..."
ssh "$host" 'dmesg 2>&1 | tail -200' > "$out/dmesg_tail.txt" 2>&1

echo "[5/8] sshd config for AllowUsers / DenyUsers / PAM..."
ssh "$host" 'cat /etc/ssh/sshd_config 2>&1 | \
             grep -vE "^\s*#|^\s*$" | head -60; \
             echo ---pam.d/sshd---; \
             cat /etc/pam.d/sshd 2>&1; \
             echo ---pam.d/login---; \
             cat /etc/pam.d/login 2>&1 | head -30' > "$out/sshd_pam_config.txt" 2>&1

echo "[6/8] time / access restrictions..."
ssh "$host" 'echo ---time.conf---; cat /etc/security/time.conf 2>&1 | \
             grep -vE "^\s*#|^\s*$" | head -20; \
             echo ---access.conf---; cat /etc/security/access.conf 2>&1 | \
             grep -vE "^\s*#|^\s*$" | head -20; \
             echo ---hosts.allow---; cat /etc/hosts.allow 2>&1; \
             echo ---hosts.deny---; cat /etc/hosts.deny 2>&1' \
  > "$out/access_config.txt" 2>&1

echo "[7/8] cron / timers that might do scheduled maintenance..."
ssh "$host" 'echo ---root-crontab---; crontab -l 2>&1; \
             echo ---cron.d---; ls /etc/cron.d/ /etc/cron.daily/ 2>&1; \
             echo ---systemd-timers---; \
             systemctl list-timers --all 2>&1 | head -30; \
             echo ---at-jobs---; atq 2>&1' > "$out/cron_timers.txt" 2>&1

echo "[8/8] dax state + CXL driver..."
ssh "$host" 'daxctl list 2>&1; echo ---ls-dax---; ls -la /dev/dax* 2>&1; \
             echo ---cxl-module---; lsmod 2>&1 | grep -i cxl | head -5' \
  > "$out/dax_cxl_state.txt" 2>&1

echo
echo "done. diag bundle at: $out"
ls -la "$out"
