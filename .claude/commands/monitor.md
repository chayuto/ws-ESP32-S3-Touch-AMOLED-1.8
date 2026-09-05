# Read serial output from the board

`idf.py monitor` does **not** work in non-TTY contexts (Claude Code, pipes, subshells) —
it exits immediately and looks like success. Use pyserial directly, and control the modem
lines yourself.

Usage: `/monitor [seconds]` (default 14)

**Prefer `attach.sh` (no reset) over anything here.** The pyserial recipe below resets
the board on every open; use it only for the boot banner, never within ten seconds of a
flash or another reset.

```zsh
.claude/skills/serial-capture/scripts/attach.sh 20
```

The reset-and-capture recipe is packaged as a script — prefer it over retyping the inline python:

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python \
  .claude/skills/serial-capture/scripts/capture.py --seconds 20 --out /tmp/boot.log
```

---

## The macOS DTR/RTS trap

On macOS, opening `/dev/cu.usbmodem*` **asserts DTR and RTS by default**. On the ESP32-S3's
native USB-Serial/JTAG, DTR drives GPIO0 and RTS drives EN — so a naive open drops the chip
into download mode or holds it in reset. Symptoms: the first read shows
`rst:0x15 (USB_UART_CHIP_RESET) ... waiting for download`, and every later read returns
0 bytes, while `esptool chip_id` still works fine.

Set `dtr`/`rts` to `False` **before** `open()`.

## Recipe — reset into the app, then capture from the first boot line

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -u -c "
import serial, time, sys
ser = serial.Serial()
ser.port = '/dev/cu.usbmodem3101'
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False        # DTR -> GPIO0 (boot select). Must be set BEFORE open.
ser.rts = False        # RTS -> EN (reset).
ser.open()
ser.setDTR(False); ser.setRTS(True); time.sleep(0.15); ser.setRTS(False)  # hard reset into app
end = time.time() + 14
buf = bytearray()
while time.time() < end:
    buf += ser.read(4096)
ser.close()
sys.stdout.write(buf.decode('utf-8', 'replace'))
"
```

Use the IDF venv python — it has pyserial; system python does not.

## Recipe — attach without resetting (catch a running device mid-flight)

```zsh
stty -f /dev/cu.usbmodem3101 -hupcl 2>/dev/null
dd if=/dev/cu.usbmodem3101 of=/tmp/board.log bs=1 count=60000 &
DD=$!; sleep 12; kill $DD 2>/dev/null; wait 2>/dev/null
cat /tmp/board.log
```

`stty -hupcl` stops the driver dropping DTR on close; `dd` opens at a lower level than
pyserial and does not touch the modem control lines. This misses the boot banner but is
safe on a device you do not want to reset.

## Decoding a crash backtrace

```zsh
~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e /tmp/ws-amoled-build/<project>/<project>.elf <addresses>
```

The `.elf` must be from the exact build that crashed.
