#!/bin/zsh
# Read the board's console WITHOUT resetting it.
#
#   .claude/skills/serial-capture/scripts/attach.sh [seconds] [outfile] [port]
#
# Opens the port below the modem-control layer (dd, with -hupcl so the close
# does not reset either). You see only what the board prints while attached:
# no boot banner, but heartbeats, detections, self-test lines, watchdog
# reports and panics all arrive. This is the default way to look at the board.
# Reset it only when you truly need the boot banner, and never within ten
# seconds of another reset (see SKILL.md: the board has wedged after that).
set -u
SECS=${1:-15}
OUT=${2:-/tmp/attach.log}
PORT=${3:-/dev/cu.usbmodem3101}
[ -e "$PORT" ] || { echo "no port $PORT" >&2; exit 2; }
stty -f "$PORT" 115200 -hupcl 2>/dev/null
rm -f "$OUT"
dd if="$PORT" of="$OUT" bs=1 count=4000000 2>/dev/null &
DD=$!
sleep "$SECS"
kill "$DD" 2>/dev/null; wait "$DD" 2>/dev/null
echo "$(wc -l < "$OUT" | tr -d ' ') lines -> $OUT" >&2
cat "$OUT"
