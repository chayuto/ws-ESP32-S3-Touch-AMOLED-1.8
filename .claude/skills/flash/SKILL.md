---
name: flash
description: Flash a built ESP-IDF project onto the ESP32-S3-Touch-AMOLED-1.8 without wedging it. Use instead of `idf.py flash` — same image, but the chip reboots itself via watchdog rather than a host-driven DTR/RTS reset, and the skill says what not to do in the seconds afterwards.
---

# Flashing this board

## The rule

**One reset at a time, and never a host-driven reset within ten seconds of another.**

On 2026-09-05 the board wedged twice — black screen, zero console bytes, ROM not
answering esptool, USB still enumerated — each time right after `idf.py flash`'s
post-write `hard_reset` was followed within seconds by a pyserial open (which resets
again through the same USB-Serial-JTAG DTR/RTS emulation). Only a long press on PWR
recovered it. Unplugging USB did not, because the AXP2101 keeps the system up.

That costs the person at the desk a manual power cycle every time. So:

## Flash

```zsh
idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> build
.claude/skills/flash/scripts/flash.sh <name>
```

`flash.sh` runs esptool with `--after watchdog_reset`: the chip reboots itself via its
RTC watchdog and the host never touches the modem lines after writing. It writes every
region in `flash_args`, including the ESP-SR `model` partition.

## Then look at it — without resetting

```zsh
sleep 3
.claude/skills/serial-capture/scripts/attach.sh 30
```

You miss the ROM banner and the first ~2 s of boot; you get everything after, including
self-test lines, heartbeats, watchdog reports and panics. That is enough to verify a
flash. If you genuinely need the banner (bootloader, partition table, PSRAM init), use
`capture.py` — but only once, and not within ten seconds of the flash.

## Never

- `idf.py flash` followed by `capture.py`. That is the exact sequence that wedged it.
- `idf.py flash monitor` — no TTY here anyway.
- Two `capture.py` runs back to back.

## If it wedges anyway

Tell the user: hold PWR 8–10 s, release, press once. Record it in CLAUDE.md with what
preceded it. Do not keep retrying resets; each one is a chance to wedge it again.
