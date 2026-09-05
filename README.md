# ws-ESP32-S3-Touch-AMOLED-1.8

Workspace for the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (SKU 29957) — a 1.8"
368×448 QSPI AMOLED touch board on the ESP32-S3R8, with PMU, RTC, 6-axis IMU,
audio codec and microSD.

Full board notes, authoritative pinout, and hard-won rules live in [`CLAUDE.md`](./CLAUDE.md).

## Board

| | Value |
|---|---|
| MCU | ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz, rev v0.2 |
| PSRAM | 8 MB octal, in-package, 80 MHz |
| Flash | 16 MB NOR, QIO @ 80 MHz |
| Display | 1.8" AMOLED 368×448 RGB565, QSPI — **CO5300** on V2, SH8601 on V1 |
| Touch | **CST820** @ I²C `0x15` on V2, FT3168 @ `0x38` on V1 |
| PMU | AXP2101 @ `0x34` (LiPo charge/discharge, power rails) |
| RTC | PCF85063 @ `0x51` |
| IMU | QMI8658 6-axis accel + gyro @ `0x6b` |
| Audio | ES8311 codec @ `0x18`, onboard mic + speaker amp |
| I/O expander | TCA9554 @ `0x20` (`EXIO0`–`EXIO7`; **not** in the display or touch path) |
| Storage | microSD over SDMMC 1-bit |
| Radio | Wi-Fi 4 (b/g/n) + BLE 5 |
| USB | Native USB-Serial/JTAG (GPIO 19/20) |

**This unit is a V2 board** — verified by I²C probe, not assumed. See below.

## Projects

### [`projects/bringup-01`](./projects/bringup-01/) — first-boot validation

Prints what the silicon reports, scans the shared I²C bus, names every device found,
reads back the IMU `WHO_AM_I` and PMU `CHIP_ID`, and infers the board revision from
which touch controller answers. Run it on any new unit before writing peripheral code.

**Status: verified on hardware 2026-09-05.** Builds clean on ESP-IDF v5.5.3, flashes
over native USB, and reports:

```
--- Chip ---
CPU cores       : 2
Silicon revision: v0.2
Wi-Fi STA MAC   : 90:70:69:fe:84:30
Flash size      : 16 MB
PSRAM           : initialized, 8388608 bytes total, 8386192 bytes free
Internal heap   : 372511 bytes free

--- I2C scan (SDA=15 SCL=14) ---
0x15  CST820 capacitive touch (V2 board)
0x18  ES8311 audio codec
0x20  TCA9554 I/O expander
0x34  AXP2101 PMU
0x51  PCF85063 RTC
0x6b  QMI8658 6-axis IMU
QMI8658 WHO_AM_I = 0x05 (expected 0x05, OK)
AXP2101 CHIP_ID  = 0x4a

--- Board revision ---
V2: CST820 touch @ 0x15  =>  CO5300 display controller
```

### [`projects/01_project_template`](./projects/01_project_template/) — copy this to start

Brings up the whole display stack in one call — CO5300 QSPI panel, CST820 touch,
LVGL 9.5 — and draws a tappable screen. Touches are logged to serial as well as shown,
so it can be verified without looking at the panel.

**Status: verified on hardware 2026-09-05.**

```
I (460) co5300_spi: LCD panel create success, version: 2.1.0
I (666) ESP32-S3-Touch-AMOLED-1.8: Touch CST816S 0x15 found
I (668) CST816S: IC id: 183
I (670) app: display up: 368x448, touch indev registered
I (710) app: UI drawn; tap the panel to see touch events here
```

### [`projects/02_word_book_en`](./projects/02_word_book_en/) — first application

A toddler says a word; the board shows the matching photo and says it back. Offline
speech recognition on ESP-SR MultiNet7, content as data on the SD card. Built in
milestones — **M0–M2 verified 2026-09-05:** audio, continuous recognition with no wake
word, and the word → card → chime loop, all provable over serial with nobody present. Design in
[`docs/design/02_word_book_en.md`](./docs/design/02_word_book_en.md).

## Repo layout

```
ws-ESP32-S3-Touch-AMOLED-1.8/
├── CLAUDE.md                 # authoritative board notes + critical rules
├── README.md                 # this file
├── .claude/
│   ├── commands/             # /build /flash /monitor /hardware-specs /peripherals /restore-factory
│   └── skills/               # new-project, serial-capture
├── docs/
│   ├── design/               # per-project design notes
│   ├── research/             # lab notes and surveys
│   └── internal/             # working notes (gitignored)
├── projects/
│   ├── bringup-01/           # first-boot validation
│   ├── 01_project_template/  # display + touch + LVGL starting point
│   └── 02_word_book_en/      # voice-triggered picture book (in progress)
└── ref/                      # vendor docs + vendor clone (gitignored, ~650 MB)
    ├── datasheets/  schematic/  wiki/  firmware/  demo/
```

## Getting started

### Prerequisites

- ESP-IDF **≥ v5.5, < v6.1** (BSP constraint) — we use v5.5.3 at `~/esp/esp-idf`
- USB-C cable

### Build, flash, validate

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 build
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 -p /dev/cu.usbmodem3101 flash
```

Then read the serial output — **not** with `idf.py monitor`, which exits silently in
non-TTY shells:

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python \
  .claude/skills/serial-capture/scripts/capture.py --seconds 20
```

That script works around a macOS quirk: opening `/dev/cu.usbmodem*` asserts DTR and RTS,
which are wired to GPIO0 and EN, dropping the board into download mode. See the
`serial-capture` skill for the why and for the no-python alternative.

### New project

1. `cp -R projects/01_project_template projects/<name>` — CMakeLists,
   `main/idf_component.yml`, `partitions.csv` and `sdkconfig.defaults` are board-correct.
   (Start from `bringup-01` instead if you want no display stack.)
2. `rm -rf sdkconfig sdkconfig.old dependencies.lock managed_components build` in the copy.
   A stale `sdkconfig` silently overrides anything you change in `sdkconfig.defaults`.
3. Rename the project in the top-level `CMakeLists.txt`.
4. Replace `build_ui()` in `main/app_main.c`.
5. Keep secrets out of `sdkconfig.defaults` — put them in `sdkconfig.defaults.local`
   (gitignored). See "Credential pattern" in `CLAUDE.md`.

The `new-project` skill in `.claude/skills/` walks the same steps with the reasoning
behind each one.

## Reference material

`ref/` is gitignored (~650 MB). Rebuild it with the sources listed in
[`ref/README.md`](./ref/README.md) — schematic, all seven peripheral datasheets, an
offline wiki snapshot, the vendor GitHub clone, and a full 16 MB backup of the
as-shipped factory flash.

## Vendor sources

- Product page: <https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm>
- Documentation: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8>
- Vendor repo: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8>
- BSP component: <https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8>
