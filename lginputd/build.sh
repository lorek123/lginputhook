#!/bin/sh
# Builds lginputd as a static ARMv7 binary (musl) and installs it into
# service/inputhook/, where the package build picks it up.
# Needs Docker. Usage: lginputd/build.sh
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD:/src" -w /src muslcc/x86_64:armv7l-linux-musleabihf \
	sh -c 'gcc -O2 -Wall -Wextra -Wno-format-truncation -static -o lginputd lginputd.c -lrt && strip lginputd'
install -m 755 lginputd ../service/inputhook/lginputd
rm -f lginputd
echo "built service/inputhook/lginputd"
