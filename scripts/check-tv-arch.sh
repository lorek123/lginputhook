#!/usr/bin/env bash
# Check webOS TV architecture (for ezinject: use aarch64 or arm binary).
# Prereq: Add your TV with ares-setup-device, then run this script with the device name.
# Example: ./scripts/check-tv-arch.sh myTV
# Or set default device: ares-setup-device -s myTV  then  ./scripts/check-tv-arch.sh

set -e
DEVICE="${1:-}"

if [ -z "$DEVICE" ]; then
  echo "Usage: $0 <device-name>"
  echo ""
  echo "Configured devices:"
  ares-device -D
  echo ""
  echo "Example: $0 livingRoomTV"
  echo "Or add a device first:"
  echo "  ares-setup-device --add livingRoomTV -i \"host=192.168.1.xxx\" -i \"port=22\" -i \"username=root\" -i \"password=alpine\""
  exit 1
fi

echo "Device: $DEVICE"
echo "---"
echo "System info (may include architecture):"
ares-device -i -d "$DEVICE" || true
echo "---"
echo "If ares-shell is available for your profile, uname would be:"
ares-shell -d "$DEVICE" -r "uname -m" 2>/dev/null || echo "(ares-shell not supported for TV profile - use SSH below)"
echo "---"
echo "To get architecture via SSH (use TV IP from ares-setup-device):"
echo "  ssh root@<TV_IP> uname -m"
echo "  (password often: alpine when Developer Mode is on)"
