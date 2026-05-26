#!/usr/bin/env bash
# Fetch inputhook logs from the TV.
# Usage: ./scripts/tv-logs.sh [lines]   (default: 50)
set -euo pipefail

TV_HOST="${TV_HOST:-192.168.1.188}"
TV_USER="${TV_USER:-root}"
LINES="${1:-50}"

SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no ${TV_USER}@${TV_HOST}"

for LOG in ezinject-lginput2 ezinject-tvservice hookfactory-lginput2 hookfactory-tvservice lginput-hook-lginput2 lginput-hook-tvservice; do
    echo "=== /tmp/${LOG}.log (last ${LINES} lines) ==="
    $SSH "tail -${LINES} /tmp/${LOG}.log 2>/dev/null" || echo "(not found)"
    echo ""
done
