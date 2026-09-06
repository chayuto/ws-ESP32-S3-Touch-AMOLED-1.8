#!/bin/zsh
# Send one command character to the board without resetting it.
#
#   .claude/skills/serial-capture/scripts/send.sh <char> [port]
#
# A bare `printf 'm' > /dev/cu.usbmodem3101` opens the port, writes and closes with
# hang-up; when the port was already closed before, the byte can be lost (2026-09-06:
# two maintenance "leave" commands vanished this way and the board sat in maintenance,
# not listening). This holds the port open with -hupcl, gives the line a moment before
# and after the byte, and closes without dropping DTR. Safe to use while attach.sh runs.
set -u
CMD=${1:?command character}
PORT=${2:-/dev/cu.usbmodem3101}
[ -e "$PORT" ] || { echo "no port $PORT" >&2; exit 2; }
stty -f "$PORT" 115200 -hupcl 2>/dev/null
exec 3<>"$PORT"
sleep 0.3
printf '%s' "$CMD" >&3
sleep 0.3
exec 3>&-
