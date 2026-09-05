#!/bin/zsh
# Flash a built project WITHOUT the post-write DTR/RTS reset.
#
#   .claude/skills/flash/scripts/flash.sh <project-name> [port]
#
# Uses esptool directly with --after watchdog_reset: the chip reboots itself
# through its RTC watchdog instead of the host toggling RTS/DTR through the
# USB-Serial-JTAG peripheral. idf.py flash uses hard_reset, and twice on
# 2026-09-05 the board wedged (black screen, no console, ROM silent, only a
# PWR long-press recovered it) right after a hard_reset followed by another
# host-side reset. Do not open the port for at least three seconds after this
# script returns, and then attach without resetting (serial-capture skill).
set -eu
NAME=${1:?project name, e.g. 02_word_book_en}
PORT=${2:-/dev/cu.usbmodem3101}
BUILD=/tmp/ws-amoled-build/$NAME
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
[ -f "$BUILD/flash_args" ] || { echo "no $BUILD/flash_args - build first" >&2; exit 2; }
# Refuse a stale binary: a failed build leaves the previous .bin in place, and flashing
# that silently tests the wrong code. Compare against the newest source in the project
# and the shared components.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
NEWEST=$(find "$REPO/projects/$NAME/main" "$REPO/projects/$NAME/sdkconfig.defaults" "$REPO/components" -type f -not -name '*.o' 2>/dev/null | xargs stat -f '%m' 2>/dev/null | sort -n | tail -1)
BIN_T=$(stat -f '%m' "$BUILD/$NAME.bin")
if [ -n "$NEWEST" ] && [ "$BIN_T" -lt "$NEWEST" ]; then
  echo "REFUSING: $BUILD/$NAME.bin ($(date -r "$BIN_T" '+%H:%M:%S')) is older than the newest source ($(date -r "$NEWEST" '+%H:%M:%S')). Build first." >&2
  exit 3
fi
echo "binary built $(date -r "$BIN_T" '+%Y-%m-%d %H:%M:%S')" >&2
cd "$BUILD"
$PY -m esptool --chip esp32s3 --port "$PORT" --baud 921600 --connect-attempts 30 \
  --before default_reset --after watchdog_reset write_flash @flash_args
echo "flashed $NAME; chip is rebooting via watchdog. Wait >=3 s, then attach.sh - do not reset it." >&2
