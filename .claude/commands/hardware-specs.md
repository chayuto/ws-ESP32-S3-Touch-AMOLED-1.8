# ESP32-S3-Touch-AMOLED-1.8 hardware specs (authoritative)

Read this before writing code that touches peripherals, memory, Wi-Fi, display, or audio.
Every number below is either from this unit's boot log or from the BSP header
`managed_components/waveshare__esp32_s3_touch_amoled_1_8/include/bsp/esp32_s3_touch_amoled_1_8.h`
(component `waveshare/esp32_s3_touch_amoled_1_8` v2.0.3).

---

## Board revision — check this first

Two revisions ship under the same name:

| | V1 | **V2 (this unit)** |
|---|---|---|
| Display controller | SH8601 | **CO5300** |
| Touch controller | FT3168 @ `0x38` | **CST820 @ `0x15`** |

`projects/bringup-01` probes I²C and prints the verdict. BSP v2.0.3 hard-codes the CO5300
panel driver, so a V1 board needs `esp_lcd_sh8601` swapped in — the BSP will not tell you,
because it only auto-detects the *touch* chip.

## Chip

- **ESP32-S3** (QFN56), Xtensa LX7, **dual-core** — `xTaskCreatePinnedToCore()` is valid
- Silicon rev **v0.2**, efuse block rev v1.4
- **240 MHz only if configured** — IDF defaults to 160 MHz; set `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`
- Wi-Fi 4 (b/g/n) + BLE 5. **No 802.15.4** (the C6 sibling has it; this does not)
- Wi-Fi STA MAC of this unit: `90:70:69:fe:84:30`

## Memory

- **SRAM:** 512 KB on-chip. After boot ≈ **372 KB** free for dynamic allocation
  (326 KiB + 21 KiB + 32 KiB DRAM + 7 KiB RTCRAM)
- **PSRAM:** **8 MB octal**, in-package (S3R8). AP Memory gen-3, 64 Mbit, 80 MHz, 3 V,
  hybrid-wrap 32-byte burst, 10-cycle fixed read latency. `Adding pool of 8192K` at boot;
  **8,386,192 bytes free** to the heap.
- **Flash:** **16 MB** external NOR, QIO @ 80 MHz, 3.3 V

A full-screen RGB565 framebuffer is 368 × 448 × 2 = **330 KB**. That does not fit
comfortably in internal RAM — put LVGL and panel buffers in PSRAM.

## I²C bus (single, shared)

**SDA = GPIO 15, SCL = GPIO 14.** 400 kHz by default (`CONFIG_BSP_I2C_FAST_MODE=y`),
port `I2C_NUM_1` (`CONFIG_BSP_I2C_NUM`).

| Address | Device | Identity register |
|---|---|---|
| `0x15` | CST820 touch (V2) | — |
| `0x18` | ES8311 audio codec | — |
| `0x20` | TCA9554 I/O expander | — |
| `0x34` | AXP2101 PMU | `CHIP_ID` reg `0x03` = `0x4a` |
| `0x38` | FT3168 touch (V1 only — absent on this unit) | — |
| `0x51` | PCF85063 RTC | — |
| `0x6b` | QMI8658 IMU | `WHO_AM_I` reg `0x00` = `0x05` |

`bsp_i2c_init()` logs `Please check pull-up resistances...`. Expected — the board has
hardware pull-ups and all devices enumerate.

## Display (QSPI AMOLED)

| Signal | GPIO |
|---|---|
| PCLK | 11 |
| CS | 12 |
| D0 / D1 / D2 / D3 | 4 / 5 / 6 / 7 |
| RST | none (`GPIO_NUM_NC`) |
| Backlight | none (`GPIO_NUM_NC`) — AMOLED is self-emitting |

- Host: `SPI2_HOST`. Resolution 368×448, RGB565, 16 bpp, RGB element order, little-endian.
- **Brightness is a controller register**, not a PWM pin: `bsp_display_brightness_set(pct)`.
- Reset lines run through the TCA9554, so `bsp_io_expander_init()` must come first.

## Touch

- INT on **GPIO 21**. No reset GPIO (`BSP_LCD_TOUCH_RST` is `GPIO_NUM_NC`).
- BSP probes CST816S's address first, then FT5x06, and picks the driver that answers.
  The CST820 is register-compatible with the CST816S driver.
- The CST816S path applies an X gap of `0x10` (16 px) — the panel's active area is offset.

## Audio (ES8311)

| Signal | GPIO |
|---|---|
| MCLK | 16 |
| BCLK (SCLK) | 9 |
| LRCLK (WS) | 45 |
| DOUT (→ codec DAC) | 8 |
| DSIN (← mic ADC) | 10 |
| Speaker amp enable | 46 |

I²S port `I2C_NUM_1`-adjacent `CONFIG_BSP_I2S_NUM`. Both speaker and mic are supported
(`BSP_CAPS_AUDIO_SPEAKER` and `BSP_CAPS_AUDIO_MIC` are 1).

## microSD (SDMMC, 1-bit)

| Signal | GPIO |
|---|---|
| CMD | 1 |
| CLK | 2 |
| D0 | 3 |

Mount point `/sdcard` (`CONFIG_BSP_SD_MOUNT_POINT`). Unlike the C6 sibling — which has no
native SDMMC host and must use SPI — this board uses the real SDMMC peripheral.

## Power

- **AXP2101 PMU** handles LiPo charge/discharge and the board's power rails. The battery
  connects via MX1.25. Read `ref/datasheets/AXP2101.pdf` before touching any rail; a wrong
  register can brown out the display or the card slot.
- **PCF85063 RTC** has its own backup-battery pads.

## Other BSP capabilities

`BSP_CAPS_BUTTONS` is **0** — there is no user button exposed through the BSP.
`BSP_CAPS_IMU` is **0** — the QMI8658 is on the bus but the BSP provides no driver for it;
talk to it directly, or copy the vendor example `92_qmi8658_imu`.
