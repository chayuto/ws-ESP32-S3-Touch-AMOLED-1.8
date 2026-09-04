# Restore the shipped factory firmware

The board ships running Waveshare's `esp-brookesia` launcher. Any flash overwrites it.
Two ways back.

Usage: `/restore-factory` (this unit's own backup) or `/restore-factory vendor`.

## Option 1 — this unit's own 16 MB backup (preferred, exact)

`ref/firmware/factory-backup-16MB-20260905.bin` is a full dump of **this board's** flash
taken before it was ever reflashed (MAC `90:70:69:fe:84:30`, sha256 `6f188fb9…`). It
includes the bootloader, the Waveshare partition table, the app, and the populated
`assets`/`storage` SPIFFS partitions — so calibration and asset data come back too.

```zsh
. ~/esp/esp-idf/export.sh 2>/dev/null
PORT=$(ls /dev/cu.usbmodem* | head -1)

python -m esptool --chip esp32s3 --port "$PORT" -b 921600 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode qio --flash_size 16MB --flash_freq 80m \
    0x0 ref/firmware/factory-backup-16MB-20260905.bin
```

Verify first if in doubt:
```zsh
shasum -a 256 ref/firmware/factory-backup-16MB-20260905.bin
# 6f188fb9d35ee793a3423934a4fa4e7c1fef9cc9dae76f9f177dabe854a6cdb3
```

## Option 2 — vendor prebuilt image

```
ref/demo/ESP32-S3-Touch-AMOLED-1.8/Firmware/
├── ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin       ← V1 boards
└── ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin    ← V2 boards (this unit)
```

**Pick the right revision.** This unit is **V2** (CST820 touch @ `0x15`, CO5300 display).
Flashing the V1 image gives a board that boots but drives the panel with the wrong
controller. Read `ref/demo/ESP32-S3-Touch-AMOLED-1.8/Firmware/README.txt` for the
vendor's own offset guidance before flashing — merged images go at `0x0`, but confirm.

## Taking a fresh backup before any risky flash

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -m esptool \
    --port /dev/cu.usbmodem3101 -b 921600 \
    read_flash 0x0 0x1000000 ref/firmware/backup-$(date +%Y%m%d).bin
```

Takes a few minutes for 16 MB; esptool writes the file at the end, so an empty file
mid-run is normal.

## If flash fails to connect

Unplug USB → hold **BOOT** → plug USB → release **BOOT** → retry.
