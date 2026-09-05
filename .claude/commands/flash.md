# Flash

**Use the `flash` skill:** `.claude/skills/flash/scripts/flash.sh <project>` after
`idf.py build`. It flashes with `--after watchdog_reset` so the host never toggles the
modem lines after writing. Then wait 3 s and `attach.sh` — do **not** run `capture.py`
straight after a flash; that sequence wedged the board twice on 2026-09-05 and needs a
PWR long-press to recover.

---

# Flash firmware to the connected ESP32-S3-Touch-AMOLED-1.8

Usage: `/flash <project_path>` (same path used with `/build`)

## Steps

1. Activate IDF: `. ~/esp/esp-idf/export.sh 2>/dev/null`

2. Detect the port. The S3 exposes **native USB-Serial/JTAG** — no USB-UART bridge,
   so it enumerates directly as `usbmodem*`:
   ```zsh
   PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
   # /dev/cu.usbmodem3101 on this host. The number is per-cable, not per-board.
   ```

3. Flash (DTR/RTS auto-enter download mode; no button press needed):
   ```zsh
   idf.py -C <project_path> -B /tmp/ws-amoled-build/$(basename <project_path>) \
          -p "$PORT" flash 2>&1 | tail -12
   ```

4. Expect the run to end with:
   ```
   Hash of data verified.
   Leaving...
   Hard resetting via RTS pin...
   Done
   ```

## Warnings

- **Never run `idf.py flash monitor` in a non-TTY shell** (piped, subshell, Claude Code).
  The monitor exits immediately, which can abort the flash mid-write. Flash first, then
  capture serial separately — see `/monitor`.
- If the port is busy, `esptool` reports "Could not open serial port". A previous serial
  reader is still attached; kill it.
- If auto-download fails, force bootloader mode: unplug USB → hold **BOOT** → plug USB →
  release **BOOT** → retry.

## This overwrites the factory firmware

The board ships running Waveshare's `esp-brookesia` launcher. Flashing replaces it.
A full 16 MB backup of this unit's as-shipped flash is at
`ref/firmware/factory-backup-16MB-20260905.bin` — restore with `/restore-factory`.
