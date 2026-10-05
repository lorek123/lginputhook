#!/bin/sh
# Keeps lginputd running and restarts it if it exits. Backs off when it
# keeps exiting quickly, so a broken setup doesn't spin.

DIR=$(dirname "$0")
LOG=/tmp/lginputd.log
PIDFILE=/tmp/lginputd-supervisor.pid

# Only one supervisor at a time.
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
	exit 0
fi
echo $$ > "$PIDFILE"

delay=2
while true; do
	started=$(date +%s)
	"$DIR/lginputd" >> "$LOG" 2>&1 < /dev/null
	status=$?
	if [ $(( $(date +%s) - started )) -ge 60 ]; then
		delay=2
	elif [ $delay -lt 60 ]; then
		delay=$(( delay * 2 ))
	fi
	echo "lginputd exited with status $status, restarting in ${delay}s" >> "$LOG"
	sleep $delay
done
