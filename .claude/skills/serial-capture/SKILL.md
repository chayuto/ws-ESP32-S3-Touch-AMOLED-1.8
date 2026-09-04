---
name: serial-capture
description: Read the serial console of the ESP32-S3-Touch-AMOLED-1.8 board. Use whenever you need boot logs, ESP_LOG output, a panic backtrace, or proof that flashed firmware is running — and instead of `idf.py monitor`, which cannot work in a non-TTY shell. Covers the macOS DTR/RTS trap that otherwise drops the board into download mode.
---

# Serial capture

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
`--seconds`, `--no-reset` to attach to a running app without disturbing it.

## Attach without resetting, no python

```zsh
stty -f /dev/cu.usbmodem3101 115200 -hupcl
dd if=/dev/cu.usbmodem3101 bs=1 count=4000 2>/dev/null
```

`-hupcl` stops the close from resetting the board. You will only see output produced
while attached — no boot banner. Good for watching a heartbeat or catching a tap.

## Reading the result

A healthy boot on this board is documented in `CLAUDE.md` under "Expected log lines".
Two warnings are normal and should not be chased: the `i2c.master` pull-up notice and
`co5300_spi: The 3Ah command has been used...`. Anything else at `W` or above deserves
a look.

To decode a panic backtrace:

```zsh
xtensa-esp32s3-elf-addr2line -pfiaC -e /tmp/ws-amoled-build/<name>/<name>.elf <addresses>
```
