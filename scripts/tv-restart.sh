#!/usr/bin/env bash
# Restart the inputhook service on the TV.
# Usage: ./scripts/tv-restart.sh
set -euo pipefail

TV_HOST="${TV_HOST:-192.168.1.188}"
TV_USER="${TV_USER:-root}"

SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no ${TV_USER}@${TV_HOST}"

echo "==> Stopping service..."
$SSH "rm -f /tmp/inputhook; pkill -f 'node.*inputhook' 2>/dev/null || true" || true

echo "==> Starting service via luna-send..."
$SSH "luna-send -n 1 'luna://org.webosbrew.inputhook.service/start' '{}'" || true

echo "==> Done. Waiting 3s then checking logs..."
sleep 3
$SSH "echo '--- ezinject-lginput2 (last 20) ---'; tail -20 /tmp/ezinject-lginput2.log 2>/dev/null; echo ''; echo '--- hookfactory-lginput2 (last 10) ---'; tail -10 /tmp/hookfactory-lginput2.log 2>/dev/null; echo ''; echo '--- lginput-hook-lginput2 (last 10) ---'; tail -10 /tmp/lginput-hook-lginput2.log 2>/dev/null" || true
