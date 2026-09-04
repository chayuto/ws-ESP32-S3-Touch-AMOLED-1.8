# CLAUDE.md — ws-ESP32-S3-Touch-AMOLED-1.8

Workspace for the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (SKU 29957; `-EN` variant is 29958).

A 1.8" 368×448 QSPI AMOLED touch board built on the ESP32-S3R8, with AXP2101 PMU,
PCF85063 RTC, QMI8658 IMU, ES8311 audio, TCA9554 I/O expander, and a microSD slot.

## This unit (verified on hardware 2026-09-05)

**It is a V2 board.** The board ships in two revisions and they differ in the two
parts you touch first:

| | V1 | **V2 (this unit)** |
|---|---|---|
| Display controller | SH8601 | **CO5300** |
| Touch controller | FT3168 @ I²C 0x38 | **CST820 @ I²C 0x15** |

Determined by I²C probe, not by guessing — `projects/bringup-01` prints the verdict.
Re-run it on any new unit before assuming the revision.

Identity of this specific board:

- Wi-Fi STA MAC `90:70:69:fe:84:30`
- Serial port on this host: `/dev/cu.usbmodem3101` (native USB-Serial/JTAG; number is per-cable)
- Shipped factory app: `esp-brookesia`, built 2026-05-27 against IDF v5.5.4-dirty

## Board Facts (verified from boot logs, not the datasheet)

- **Chip:** ESP32-S3 (QFN56) rev **v0.2**, efuse block rev v1.4, dual-core Xtensa LX7
- **CPU:** 240 MHz — **only if you set it**; the IDF default is 160 MHz (see Critical Rules)
- **PSRAM:** 8 MB octal, in-package (AP Memory gen-3, 64 Mbit, 80 MHz, 3V, 10-cycle
  fixed read latency). Boot log: `Adding pool of 8192K of PSRAM memory to heap allocator`;
  8,386,192 bytes free to the heap after boot.
- **Flash:** 16 MB external NOR, **QIO @ 80 MHz**, 3.3 V (eFuse says quad, 4 data lines)
- **Internal SRAM after boot:** ~372 KB free (326 KiB + 21 KiB + 32 KiB DRAM + 7 KiB RTCRAM)
- **USB:** native USB-Serial/JTAG (GPIO 19/20) — enumerates as `/dev/cu.usbmodem*` directly
- **Radio:** Wi-Fi 4 (b/g/n) + BLE 5. **No 802.15.4** (unlike the C6 sibling board)
- **Display:** 1.8" AMOLED, 368×448, RGB565, QSPI on SPI2_HOST. Self-emitting —
  there is **no backlight pin**; brightness is a display-controller register.
- **Power:** AXP2101 PMU (CHIP_ID `0x4a`), LiPo charge/discharge, MX1.25 connector

## Authoritative Pinout

Verified against the BSP header
`managed_components/waveshare__esp32_s3_touch_amoled_1_8/include/bsp/esp32_s3_touch_amoled_1_8.h`
(component `waveshare/esp32_s3_touch_amoled_1_8` v2.0.3) and confirmed live by `bringup-01`.

| Bus / Function | GPIO(s) |
|---|---|
| I²C SCL / SDA (shared: AXP2101, TCA9554, ES8311, PCF85063, QMI8658, touch) | 14 / 15 |
| I²S MCLK / BCLK / LRCLK | 16 / 9 / 45 |
| I²S DOUT (→ ES8311 DAC) / DSIN (← mic ADC) | 8 / 10 |
| Speaker amplifier enable | 46 |
| LCD QSPI PCLK / CS | 11 / 12 |
| LCD QSPI D0 / D1 / D2 / D3 | 4 / 5 / 6 / 7 |
| LCD RST / backlight / touch RST | **none** — `GPIO_NUM_NC` (AMOLED; reset via I/O expander) |
| Touch INT | 21 |
| SD card CMD / CLK / D0 (SDMMC 1-bit) | 1 / 2 / 3 |
| USB D- / D+ | 19 / 20 |

## I²C Bus — what actually answers

Single shared bus, **SDA = GPIO 15, SCL = GPIO 14**, 400 kHz by default
(`CONFIG_BSP_I2C_FAST_MODE=y`). Live scan from this unit:

| Address | Device |
|---|---|
| `0x15` | CST820 capacitive touch (**V2**; a V1 board answers at `0x38` instead) |
| `0x18` | ES8311 audio codec |
| `0x20` | TCA9554 I/O expander |
| `0x34` | AXP2101 PMU — `CHIP_ID` (reg `0x03`) = `0x4a` |
| `0x51` | PCF85063 RTC |
| `0x6b` | QMI8658 6-axis IMU — `WHO_AM_I` (reg `0x00`) = `0x05` |

The BSP logs `Please check pull-up resistances...` on init. That warning is expected —
the board has hardware pull-ups and every device above still enumerates.

## Repo Layout

```
ws-ESP32-S3-Touch-AMOLED-1.8/
├── CLAUDE.md                 # this file — authoritative board notes
├── README.md
├── .claude/commands/         # /build /flash /monitor /hardware-specs /peripherals /restore-factory
├── .gitignore                # excludes /ref/, build/, managed_components/
├── docs/
│   ├── research/             # public-info-sourced surveys, design + method docs
│   └── internal/             # working notes (gitignored)
├── projects/
│   └── bringup-01/           # first-boot validation: chip report + I²C scan + revision detect
└── ref/                      # vendor material (gitignored, ~650 MB)
    ├── datasheets/           # ESP32-S3, AXP2101, QMI8658C, PCF85063A, ES8311, CO5300, SH8601A0, FT3168
    ├── schematic/            # ESP32-S3-Touch-AMOLED-1.8-schematic.pdf
    ├── wiki/                 # offline HTML snapshot of docs.waveshare.com
    ├── firmware/             # full 16 MB backup of the as-shipped flash
    └── demo/ESP32-S3-Touch-AMOLED-1.8/   # official vendor repo clone
```

## ESP-IDF Environment

- **Version required:** ≥ v5.5, < v6.1 (BSP constraint). We use **v5.5.3** at `~/esp/esp-idf`.
- **Activate:** `. ~/esp/esp-idf/export.sh` (source it; Claude Code prompts for the `.` builtin once per session)
- **Python venv:** `~/.espressif/python_env/idf5.5_py3.14_env/bin/python` — has pyserial; system python does not.

## Build & Flash

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 build
idf.py -C projects/bringup-01 -B /tmp/ws-amoled-build/bringup-01 -p /dev/cu.usbmodem3101 flash
```

Out-of-tree build dirs (`-B /tmp/...`) keep vendor clones and the repo clean.
`idf.py monitor` does **not** work in non-TTY shells — see `/monitor`.

## Critical Rules

- **Set the CPU frequency explicitly.** ESP-IDF defaults to 160 MHz on the S3. The first
  bringup boot logged `cpu freq: 160000000 Hz` with the vendor's own `sdkconfig.defaults`.
  Add `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` or you silently give up a third of the clock.
- **Always pass `-C <project>`** — bare `idf.py` operates on the cwd.
- **Never assume the board revision.** BSP v2.0.3 hard-codes the **CO5300** panel driver
  but probes for touch at runtime (CST816S address first, then FT5x06). So the BSP will
  bring up touch on a V1 board while driving the panel with the wrong controller. If you
  ever meet a V1 unit, the display path needs `esp_lcd_sh8601`, not `esp_lcd_co5300`.
- **There is no backlight GPIO.** `BSP_LCD_BACKLIGHT`, `BSP_LCD_RST` and `BSP_LCD_TOUCH_RST`
  are all `GPIO_NUM_NC`. Brightness goes through `bsp_display_brightness_set()`, which
  writes a display-controller register. The panel is driven with no reset GPIO at all.
- **The TCA9554 is not in the display or touch path.** It sits at `0x20` and the BSP
  exposes `bsp_io_expander_init()`, but the BSP never calls it: `bsp_display_start()`
  brings up the panel and touch without it — verified by `01_project_template`, which
  never touches the expander and still gets a working display and a registered indev.
  Its eight lines are labelled `EXIO0`–`EXIO7` on the schematic (a `DSI_PWR_EN` net sits
  in the same area); what each drives is not yet established. Call `bsp_io_expander_init()`
  only when you need those lines. Do **not** carry over the C6 sibling's rule that the
  expander gates the display and touch rails — true on that board, not this one.
- **The AXP2101 owns the power rails.** Misconfiguring it can brown out the display or the
  card slot. Read `ref/datasheets/AXP2101.pdf` before changing any rail, and prefer the
  vendor example `90_axp2101_pmu` as the reference sequence.
- **ESP32-S3 is dual-core** — `xTaskCreatePinnedToCore()` is valid here, unlike on the
  single-core C6 sibling boards.
- **PSRAM is 8 MB octal** — full framebuffers and LVGL buffers belong there.
  368×448×2 bytes = 330 KB per full-screen RGB565 buffer, which does not fit comfortably
  in internal RAM.
- **Restoring the shipped firmware is possible** — the as-shipped 16 MB image is backed up
  at `ref/firmware/factory-backup-16MB-20260905.bin`. See `/restore-factory`.

## Credential pattern

`sdkconfig.defaults` is **committed** and must contain no secrets. Real Wi-Fi or API
credentials go in `projects/<name>/sdkconfig.defaults.local`, which is gitignored, and are
layered in at build time:

```zsh
idf.py -C projects/<name> -B <build> \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
```

## Vendor Example Index (`ref/demo/ESP32-S3-Touch-AMOLED-1.8/examples/esp-idf/`)

| Example | Purpose |
|---|---|
| `00_board_check` | Chip/BSP capability report (the seed for our `bringup-01`) |
| `00_bsp_quickstart` | Minimal BSP init |
| `01_project_template` | Empty project scaffold |
| `02_hello_world`, `03_nvs_counter`, `04_freertos_tasks` | IDF basics |
| `05_gpio_io`, `06_gpio_interrupt` | GPIO |
| `08_i2c_tools` | I²C bus scanner |
| `09_sdmmc` | microSD over SDMMC |
| `10_wifi_station` | Wi-Fi STA |
| `12_i2s_codec` | ES8311 playback / capture |
| `13_display_colorbar` | Raw panel test |
| `14_lvgl_demo_v9` | LVGL 9 demo |
| `90_axp2101_pmu`, `91_pcf85063_rtc`, `92_qmi8658_imu` | Per-peripheral references |

Arduino sets live in `examples/arduino/` (V1) and `examples/arduino-v2/` (V2).

## Differences from Sibling Repos

| | **this repo** | `ws-ESP32-S3-CAM` | `ESP32-C6-Touch-AMOLED-1.8` |
|---|---|---|---|
| Target | `esp32s3` | `esp32s3` | `esp32c6` |
| Cores | 2 (Xtensa LX7) | 2 (Xtensa LX7) | 1 (RISC-V) |
| PSRAM | 8 MB octal | 8 MB octal | none |
| Display | Onboard AMOLED 368×448 (CO5300) | External FPC LCD only | Onboard AMOLED 368×448 (SH8601) |
| Camera | none | GC2145 DVP | none |
| PMU | AXP2101 | none (plain Li charger) | AXP2101 |
| SD | SDMMC 1-bit (1/2/3) | SDMMC 1-bit (43/16/44) | SPI only (no SDMMC host on C6) |
| I²C | SDA 15 / SCL 14 | SDA 8 / SCL 7 | SDA 8 / SCL 7 |

The C6 board is the closest relative — same panel size and resolution, same PMU/RTC/IMU
family — but a different display controller, different pins, and a single RISC-V core.
Port code from it by concept, never by pin number.
