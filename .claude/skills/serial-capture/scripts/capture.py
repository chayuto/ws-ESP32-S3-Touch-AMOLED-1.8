#!/usr/bin/env python
"""Capture serial console output from the ESP32-S3-Touch-AMOLED-1.8 on macOS.

Run with the ESP-IDF venv python, which has pyserial:
    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python capture.py --seconds 20

Why this exists: opening /dev/cu.usbmodem* on macOS asserts DTR and RTS, which on
this board are wired to GPIO0 and EN. A naive open() drops the chip into download
mode and you capture nothing. Both lines must be deasserted BEFORE open().

Limitation, verified on hardware 2026-09-05: deasserting them avoids download mode
but does NOT avoid the reset. Every open() through pyserial restarts the app, so
this script always gives you a fresh boot and always destroys accumulated run state
(counters, uptime). To observe a running app without disturbing it, use the stty/dd
recipe in SKILL.md, which opens below the modem-control layer.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/cu.usbmodem3101",
                    help="serial port (the number is per-cable; check `ls /dev/cu.usbmodem*`)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0,
                    help="how long to capture for")
    ap.add_argument("--no-reset", action="store_true",
                    help="skip the deliberate reset pulse. NOTE: this does NOT give you a "
                         "non-resetting attach — macOS still resets the board on open(). "
                         "Verified on hardware. For a true attach, use the stty/dd recipe "
                         "in SKILL.md.")
    ap.add_argument("--out", default="-",
                    help="write to this file instead of stdout")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    # Must be set before open(): DTR -> GPIO0 (boot select), RTS -> EN (reset).
    ser.dtr = False
    ser.rts = False
    ser.open()

    if not args.no_reset:
        # Hard reset into the app: pulse EN low while GPIO0 stays high.
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.15)
        ser.setRTS(False)

    end = time.time() + args.seconds
    buf = bytearray()
    try:
        while time.time() < end:
            buf += ser.read(4096)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    text = buf.decode("utf-8", "replace")
    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as fh:
            fh.write(text)
        print(f"{len(text.splitlines())} lines -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
