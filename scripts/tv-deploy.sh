#!/usr/bin/env bash
# Deploy lginputhook to TV via SSH.
# Usage: ./scripts/tv-deploy.sh [--skip-build]
set -euo pipefail

TV_HOST="${TV_HOST:-192.168.1.188}"
TV_USER="${TV_USER:-root}"
SERVICE_DIR="/media/developer/apps/usr/palm/services/org.webosbrew.inputhook.service"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# The service files live under dist/service/ (webpack output mirrors the source layout).
DIST_SERVICE_DIR="${PROJECT_DIR}/dist/service"

SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no ${TV_USER}@${TV_HOST}"
SCP="scp -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no"

if [[ "${1:-}" != "--skip-build" ]]; then
    echo "==> Building..."
    cd "$PROJECT_DIR"
    npm run build
    echo ""
fi

echo "==> Stopping service on TV..."
$SSH "rm -f /tmp/inputhook; pkill -f 'node.*inputhook' 2>/dev/null || true" || true

echo "==> Cleaning old files on TV..."
$SSH "rm -rf ${SERVICE_DIR}/inputhook ${SERVICE_DIR}/interface ${SERVICE_DIR}/service ${SERVICE_DIR}/service.js ${SERVICE_DIR}/services.json ${SERVICE_DIR}/package.json"

echo "==> Syncing service files to TV..."
$SCP -r "${DIST_SERVICE_DIR}"/* "${TV_USER}@${TV_HOST}:${SERVICE_DIR}/"

echo "==> Setting permissions..."
$SSH "chmod 777 ${SERVICE_DIR}/inputhook/ezinject 2>/dev/null; chmod 755 ${SERVICE_DIR}/inputhook/ezinject-* 2>/dev/null || true"

echo "==> Verifying..."
$SSH "ls -la ${SERVICE_DIR}/ && echo '' && head -1 ${SERVICE_DIR}/service.js"

echo ""
echo "==> Done. Launch the app from the TV or run:"
echo "    ./scripts/tv-restart.sh"
