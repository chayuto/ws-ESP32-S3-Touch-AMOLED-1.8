# bringup-01 — first-boot validation

The first thing to flash on a new ESP32-S3-Touch-AMOLED-1.8. It answers three questions
before you write any peripheral code:

1. **Is the silicon what the box claims?** Cores, revision, flash size, PSRAM size, free heap.
2. **Is every peripheral actually on the bus?** An i2cdetect-style scan of the shared I²C
   bus with each address named, plus identity-register reads for the two parts that
   publish one (QMI8658 `WHO_AM_I`, AXP2101 `CHIP_ID`).
3. **Which board revision is this?** V1 and V2 ship different display and touch chips.
   The program infers the revision from which touch controller answers — V1 FT3168 at
   `0x38`, V2 CST820 at `0x15` — rather than assuming.

That third question is the point. BSP v2.0.3 hard-codes the CO5300 (V2) panel driver while
auto-detecting only the *touch* chip, so a V1 board will bring up touch happily and then
drive the panel with the wrong controller. Know the revision first.

## Build, flash, read

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 build
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 -p /dev/cu.usbmodem3101 flash
```

Then capture serial with the recipe in `.claude/commands/monitor.md` — `idf.py monitor`
silently does nothing in a non-TTY shell.

## Verified output (this unit, 2026-09-05)

```
--- Chip ---
IDF target      : esp32s3
IDF version     : v5.5.3
CPU cores       : 2
Silicon revision: v0.2
Wi-Fi STA MAC   : 90:70:69:fe:84:30
Features: Wi-Fi BLE
Flash size      : 16 MB
PSRAM           : initialized, 8388608 bytes total, 8386192 bytes free
Internal heap   : 372511 bytes free

--- BSP capabilities and pins ---
Display   : yes, 368x448
Touch     : yes
Speaker   : yes
Microphone: yes
SD card   : yes
I2C       : SDA=15 SCL=14
SDMMC     : CMD=1 CLK=2 D0=3

--- I2C scan (SDA=15 SCL=14) ---
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- --
10: -- -- -- -- -- 15 -- -- 18 -- -- -- -- -- -- --
20: 20 -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
30: -- -- -- -- 34 -- -- -- -- -- -- -- -- -- -- --
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
50: -- 51 -- -- -- -- -- -- -- -- -- -- -- -- -- --
60: -- -- -- -- -- -- -- -- -- -- -- 6b -- -- -- --
70: -- -- -- -- -- -- -- --

--- Devices identified ---
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

`cpu freq: 240000000 Hz` in the boot log — but only because `sdkconfig.defaults` sets
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`. The first build, using the vendor's defaults,
booted at 160 MHz.

## Notes

- The BSP logs `i2c.master: Please check pull-up resistances whether be connected
  properly` during `bsp_i2c_init()`. Expected on this board — hardware pull-ups are
  present and all six devices enumerate.
- The scan skips reserved addresses (below `0x08`, above `0x77`).
- This program does **not** initialise the display. It deliberately stops at the I²C
  layer so that a bus or power fault is diagnosed before the panel is in the picture.
  Note that the touch rail is gated by the TCA9554, so on a board where the expander has
  not been configured, touch may not answer until the display is brought up.

## Use as a scaffold

`CMakeLists.txt`, `main/idf_component.yml` and `sdkconfig.defaults` are board-correct.
Copy the directory, rename the project in the top-level `CMakeLists.txt`, and start there.
