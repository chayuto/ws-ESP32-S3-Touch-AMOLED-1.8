---
name: serial-capture
description: Read the serial console of the ESP32-S3-Touch-AMOLED-1.8 board. Use whenever you need boot logs, ESP_LOG output, a panic backtrace, or proof that flashed firmware is running — and instead of `idf.py monitor`, which cannot work in a non-TTY shell. Covers the macOS DTR/RTS trap that otherwise drops the board into download mode.
---

# Serial capture

## First rule: attaching is free, resetting is not

`capture.py` **resets the board every time it opens the port** — there is no way around
that with pyserial on macOS. `attach.sh` does not. The board wedged twice on 2026-09-05
after a reset that followed another reset within seconds, and only a PWR long-press by a
person at the desk brought it back. So:

- Default to **`attach.sh`**. It shows everything the app prints from the moment you
  attach: heartbeats, detections, self-tests, watchdog reports, panics.
- Use `capture.py` only when you need the **boot banner** itself, and never within ten
  seconds of a flash or another capture.
- After flashing, wait three seconds, then `attach.sh`. Never `capture.py`.

```zsh
.claude/skills/serial-capture/scripts/attach.sh 20            # 20 s, prints to stdout
.claude/skills/serial-capture/scripts/attach.sh 60 /tmp/x.log # longer, keep the file
```

## The trap

The board's console is the ESP32-S3's **native USB-Serial/JTAG** on GPIO 19/20, and it
enumerates on macOS as `/dev/cu.usbmodem*`. Opening that device asserts **DTR** (wired to
GPIO0, the boot-select pin) and **RTS** (wired to EN, reset). A plain `open()` therefore
resets the chip *into download mode* and you capture nothing but silence.

Both lines must be set false **before** `open()`, not after. Setting them after the open
is too late — the reset has already happened.

`idf.py monitor` is also unusable here: it needs a TTY and this shell is not one. Do not
reach for it, and never run `idf.py flash monitor`.

## Capture a boot, with the banner

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python \
  .claude/skills/serial-capture/scripts/capture.py --seconds 20 --out /tmp/boot.log
```

The script deasserts both lines, then pulses RTS to hard-reset into the app, so the
capture starts at `ESP-ROM:esp32s3-...` and includes the full bootloader and heap report.
Use the system python and you get `ModuleNotFoundError: serial` — pyserial lives only in
the IDF venv.

Useful flags: `--port` (the `usbmodem` number changes per cable — `ls /dev/cu.usbmodem*`),
`--seconds`, `--out`.

**`--no-reset` does not do what its name suggests.** It skips the deliberate reset pulse,
but macOS resets the board on `open()` regardless — verified on hardware. Every pyserial
capture therefore starts from a fresh boot and **wipes accumulated run state**: counters,
uptime, anything the app has been keeping. If you need to observe a running app, use the
next recipe instead.

## Attach without resetting — `attach.sh`

`attach.sh` is this, packaged:

```zsh
stty -f /dev/cu.usbmodem3101 115200 -hupcl
dd if=/dev/cu.usbmodem3101 of=/tmp/tap.log bs=1 count=100000 2>/dev/null &
# ... interact with the board ...
pkill -f 'dd if=/dev/cu.usbmodem3101'; cat /tmp/tap.log
```

`dd` opens below the modem-control layer and `-hupcl` stops the close from resetting.

To **send a command** (`m`, `s`, `i`, ...), use `scripts/send.sh <char>`, which holds the
port open with `-hupcl` around the byte. A bare `printf 'm' > /dev/cu.usbmodem3101` works
the first time after an attach but can lose the byte once the port has been closed and
reopened (2026-09-06: two maintenance "leave" commands vanished, the board sat in
maintenance mode and was not listening). `send.sh` works while `attach.sh` is running.
Confirmed on hardware: timestamps continue from the running boot (`I (40712)` → `I (90712)`)
instead of restarting at `I (454)`, so the app keeps its state.

You see only output produced while attached — no boot banner. Use it for anything
involving a live board or a person at it. Run it in the background (or `attach.sh` with
a long duration) while they interact.

## Reading the result

A healthy boot on this board is documented in `CLAUDE.md` under "Expected log lines".
Two warnings are normal and should not be chased: the `i2c.master` pull-up notice and
`co5300_spi: The 3Ah command has been used...`. Anything else at `W` or above deserves
a look.

To decode a panic backtrace:

```zsh
xtensa-esp32s3-elf-addr2line -pfiaC -e /tmp/ws-amoled-build/<name>/<name>.elf <addresses>
```
