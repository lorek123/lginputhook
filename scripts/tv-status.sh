#!/usr/bin/env bash
# Quick status check: TV arch, running processes, log sizes, /tmp/inputhook flag.
# Usage: ./scripts/tv-status.sh
set -euo pipefail

TV_HOST="${TV_HOST:-192.168.1.188}"
TV_USER="${TV_USER:-root}"

SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no ${TV_USER}@${TV_HOST}"

echo "==> Architecture"
$SSH "uname -m"

echo ""
echo "==> /tmp/inputhook flag"
$SSH "ls -la /tmp/inputhook 2>/dev/null || echo '(not present - injection will run on next start)'"

echo ""
echo "==> Relevant processes"
$SSH "ps w 2>/dev/null | grep -E 'inputhook|lginput|node' | grep -v grep || echo '(none)'"

echo ""
echo "==> Log file sizes"
$SSH "ls -lh /tmp/ezinject*.log /tmp/hookfactory*.log /tmp/lginput-hook*.log 2>/dev/null || echo '(no logs yet)'"

echo ""
echo "==> Autostart script"
$SSH "ls -la /var/lib/webosbrew/init.d/inputhook 2>/dev/null || echo '(not installed)'"

echo ""
echo "==> Installed service files"
$SSH "ls -la /media/developer/apps/usr/palm/services/org.webosbrew.inputhook.service/ 2>/dev/null"
