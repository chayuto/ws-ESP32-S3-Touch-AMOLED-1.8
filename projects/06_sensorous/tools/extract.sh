#!/bin/zsh
# Pull the card's files off the board over Wi-Fi, without opening the slot.
#
#   tools/extract.sh [outdir] [port]
#
# Drives the whole round trip: enters maintenance mode over the serial console,
# waits for the board to print its address, downloads every file in the data
# directory, and leaves maintenance so scanning resumes. Nothing is written to
# the card and the card is never unmounted.
#
# Repeatable by design. Each file is fetched with /api/file?from=<local size>,
# so a second run downloads only the bytes written since the first and appends
# them. A file that is shorter on the board than on disk has rotated, and is
# re-fetched whole into <name>.<n> so the old generation is not overwritten.
#
# Scanning stops while maintenance is up - one antenna - so the script leaves
# maintenance even when a download fails.
set -u
REPO=${0:A:h:h:h:h}
OUT=${1:-${0:A:h:h}/data}
PORT=${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
TAP=$(mktemp -t sensorous-tap)
ATTACH=$REPO/.claude/skills/serial-capture/scripts/send.sh

[ -n "$PORT" ] && [ -e "$PORT" ] || { echo "no serial port (looked for /dev/cu.usbmodem*)" >&2; exit 2 }
mkdir -p "$OUT"

# The console tap must be running before the command is sent, or the reply to it
# is lost: this is the only way to learn the address the board picked up.
stty -f "$PORT" 115200 -hupcl 2>/dev/null
dd if="$PORT" of="$TAP" bs=1 count=4000000 2>/dev/null &
DD=$!
cleanup() { kill $DD 2>/dev/null; rm -f "$TAP" }
sleep 1

leave() {
  "$ATTACH" m "$PORT" >/dev/null 2>&1
  for i in {1..15}; do
    grep -aq "maintenance down" "$TAP" 2>/dev/null && { echo "maintenance down, scanning resumed"; return }
    sleep 1
  done
  echo "WARNING: the board did not confirm it left maintenance - check it" >&2
}

echo "asking for maintenance mode..."
"$ATTACH" m "$PORT" >/dev/null 2>&1

IP_WAIT_S=75
IP=""
for i in {1..$IP_WAIT_S}; do
  IP=$(grep -ao "maintenance up at http://[0-9.]*" "$TAP" 2>/dev/null | tail -1 | sed 's|.*http://||')
  [[ "$IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] && break
  IP=""
  # 'm' toggles: if the board was already in maintenance we just switched it off.
  if grep -aq "maintenance down" "$TAP" 2>/dev/null; then
    echo "it was already in maintenance; asking again"
    : > "$TAP"
    "$ATTACH" m "$PORT" >/dev/null 2>&1
  fi
  sleep 1
done
if [ -z "$IP" ]; then
  # Leave maintenance even though there is nothing to download. Maintenance mode
  # stops scanning, so an early exit here used to walk away from a board that had
  # given up recording - the firmware's ten-minute idle timeout was the only thing
  # that recovered it (2026-09-18). Say what the console showed, too.
  echo "the board never came up on Wi-Fi within ${IP_WAIT_S}s. Last console lines:" >&2
  tail -6 "$TAP" >&2
  leave
  cleanup
  exit 3
fi
echo "board is at http://$IP/"

LIST=$(curl -s --max-time 20 "http://$IP/api/files") || { echo "could not list files" >&2; leave; cleanup; exit 4 }
echo "$LIST" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(f"{d[\"dir\"]} - {d.get(\"free_mb\",0)} of {d.get(\"total_mb\",0)} MB free")' 2>/dev/null

echo "$LIST" | python3 -c 'import json,sys
d=json.load(sys.stdin)
for f in d.get("files",[]): print(f["name"], f["bytes"])' 2>/dev/null | while read NAME BYTES; do
  LOCAL="$OUT/$NAME"
  HAVE=0
  [ -f "$LOCAL" ] && HAVE=$(wc -c < "$LOCAL" | tr -d ' ')
  if [ "$HAVE" -gt "$BYTES" ]; then
    N=1; while [ -e "$LOCAL.$N" ]; do N=$((N+1)); done
    mv "$LOCAL" "$LOCAL.$N"
    echo "$NAME: rotated on the board (had $HAVE B, board has $BYTES B); kept the old one as $NAME.$N"
    HAVE=0
  fi
  if [ "$HAVE" -eq "$BYTES" ]; then
    echo "$NAME: up to date ($BYTES B)"
    continue
  fi
  # Into a temporary file first, and checked against the size the board reported.
  # A server-side error arrives as a short 200-or-error body, and appending that
  # to the data file would corrupt it - which is exactly what an earlier version
  # of this script did with "no such file" (2026-09-18).
  TMP=$(mktemp -t sensorous-part)
  CODE=$(curl -s -w '%{http_code}' --max-time 300 -o "$TMP" "http://$IP/api/file?name=$NAME&from=$HAVE")
  GOT=$(wc -c < "$TMP" | tr -d ' ')
  WANT=$((BYTES-HAVE))
  if [ "$CODE" != "200" ]; then
    echo "$NAME: HTTP $CODE - $(head -c 200 "$TMP")" >&2
    rm -f "$TMP"
  elif [ "$GOT" -lt "$WANT" ]; then
    # Short is not always wrong: the file grows while it is being read. Only a
    # body far smaller than asked for means the board sent something else.
    echo "$NAME: got $GOT B of the $WANT B offered - $(head -c 120 "$TMP")" >&2
    rm -f "$TMP"
  else
    cat "$TMP" >> "$LOCAL"; rm -f "$TMP"
    echo "$NAME: +$GOT B (now $(wc -c < "$LOCAL" | tr -d ' ') B)"
  fi
done

curl -s --max-time 20 "http://$IP/api/census?kind=wifi" > "$OUT/census_wifi.json" 2>/dev/null
curl -s --max-time 20 "http://$IP/api/census?kind=ble"  > "$OUT/census_ble.json"  2>/dev/null
curl -s --max-time 20 "http://$IP/api/metrics"          > "$OUT/metrics.json"     2>/dev/null
echo "census and metrics saved"

leave
cleanup
echo "data is in $OUT"
