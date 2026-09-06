# What actually shipped on this board

**Date:** 2026-09-06
**Unit:** Waveshare ESP32-S3-Touch-AMOLED-1.8 **V2**, SKU 29957, MAC `90:70:69:fe:84:30`
**Sources:** the flash dump taken before first reflash, and the vendor GitHub repo at
`7ab8f95` (2026-08-21)

## Goal

Establish what Waveshare supplies with this hardware — as a running product and as
source — rather than assuming. The immediate question was whether a complete, buildable
reference application exists for this board. It does not, and the reason is interesting.

## The factory image is the vendor's published one, byte for byte

```
6f188fb9…a6cdb3  ref/firmware/factory-backup-16MB-20260905.bin
6f188fb9…a6cdb3  ref/demo/…/Firmware/ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin
```

Full sha256 `6f188fb9d35ee793a3423934a4fa4e7c1fef9cc9dae76f9f177dabe854a6cdb3`, matching
the value the vendor records in `docs/FIRMWARE.md` for their V2 image.

This is worth having verified: the unit shipped exactly the published image, so
`/restore-factory` restores a known artefact and not a one-off.

## The shipped application

App descriptor at `0x110000` identifies **`esp-brookesia`**, built **2026-05-27** against
IDF v5.5.4-dirty. Waveshare names their images `FactoryXiaozhi` — the app is the
open-source **Xiaozhi** AI voice assistant sitting on Espressif's Brookesia launcher.
Component paths recovered from the image confirm it:

```
./components/brookesia_core/systems/phone/esp_brookesia_phone_manager.cpp
./components/brookesia_app_calculator/esp_brookesia_app_calculator.cpp
./components/brookesia_app_squareline_demo/esp_brookesia_app_squareline_demo.cpp
```

### Partition table

Dumped from offset `0x8000` with `gen_esp32part.py`. It is Waveshare-custom — the app
does not start at `0x10000`:

```
nvsfactory, data, nvs,     0x9000,   200K
nvs,        data, nvs,     0x3b000,  840K
otadata,    data, ota,     0x10d000, 8K
phy_init,   data, phy,     0x10f000, 4K
factory,    app,  factory, 0x110000, 5632K
ota_0,      app,  ota_0,   0x690000, 3M
assets,     data, spiffs,  0x990000, 3M
storage,    data, spiffs,  0xc90000, 3520K
```

**There is no `model` or `srmodels` partition.** That single absence explains the whole
architecture below.

## Finding: the factory firmware does no speech recognition on the chip

Speech models present in the image:

| model | role |
| --- | --- |
| `wn9_nihaoxiaozhi_tts` (`wn9_data`, `wn9_index`) | WakeNet9, 你好小智 — Chinese wake word |
| `vadnet1_quantized` | voice activity detection |
| `nsnet1`, `nsnet2` | noise suppression |

MultiNet is a different story. The esp-sr **library code is linked in** —
`components/multinet/ctc_decoder_v2.c`, `language_model_v2.c`,
`multinet5_quantized8.c`, the `Quantized MultiNet6:` / `Quantized MultiNet7:` format
strings, and the `%s/%s/mn5q8_%s` / `mn6_%s` / `mn7_%s` path builders — but **no MultiNet
model data is flashed**, and there is no partition that could hold it.

So the on-device pipeline stops at the wake word. Everything after it leaves the board:

- `WebsocketProtocol` and `MqttProtocol` C++ symbols
- Opus encode and decode, including `OpusTags` headers
- `http://192.168.4.1` — SoftAP provisioning
- `https://api.tenclass.net/xiaozhi/ota/` — OTA and configuration endpoint

**Wake word local, recognition remote.** This is the vendor's own answer to the question
of running speech recognition on an ESP32-S3, and it agrees with the memory arithmetic in
[`speech-stack-20260906.md`](speech-stack-20260906.md): open-vocabulary ASR does not fit
on this silicon, so nobody runs it there.

Note also that the shipped wake word is **Chinese only**. Out of the box this unit
responds to 你好小智 and nothing else.

## What Waveshare provides as source

Two categories, and they never overlap.

### 17 ESP-IDF examples — all build, all tiny

They go through the BSP component, so V1/V2 panel differences are handled for you.

| example | LOC | | example | LOC |
| --- | --- | --- | --- | --- |
| `01_project_template` | 23 | | `12_i2s_codec` | 209 |
| `14_lvgl_demo_v9` | 39 | | `00_bsp_quickstart` | 224 |
| `02_hello_world` | 41 | | `91_pcf85063_rtc` | 301 |
| `03_nvs_counter` | 53 | | `92_qmi8658_imu` | 349 |
| `04_freertos_tasks` | 59 | | `90_axp2101_pmu` | 1555 |
| `05_gpio_io` | 60 | | `00_board_check` | 76 |
| `08_i2c_tools` | 65 | | `09_sdmmc` | 82 |
| `06_gpio_interrupt` | 92 | | `10_wifi_station` | 117 |
| `13_display_colorbar` | 147 | | | |

~3,400 LOC across all 17, and `90_axp2101_pmu` is mostly a vendored driver.
`14_lvgl_demo_v9` is 39 lines that call `lv_demo_widgets()`. Their own
`docs/EXAMPLES_GUIDE.md` describes them accurately as validation demos — "draws RGB565
test bars", "validates ES8311 speaker playback".

**This is a peripheral bring-up suite, not an application.** Nothing is wired together;
there is no speech, no app framework, and no networking beyond joining an AP.

### Arduino sketches — 16 for V1, 10 for V2

V2 loses the clock, WiFi analyzer, LVGL animation, AXP ADC readout and the SquareLine
project. The V2 library set does carry `Arduino_CST816x` and no V2 example references
FT3168, so the port is correct — `04_GFX_FT3168_Image` just kept a stale name.

### The one complete application is a blob

`docs/FIRMWARE.md`, verbatim:

> Their corresponding source and build instructions are not included in this repository
> yet and may be added in a later update.

## Conclusion

The only full, working, polished project for this hardware is a 16 MB binary you can
restore but cannot read, modify, or learn from. Everything Waveshare ships as source is a
single-peripheral demo.

Waveshare did not write that application, though — they ported it. Both halves are open
source upstream: `esp-brookesia` from Espressif, and `xiaozhi-esp32` as a community
project with board ports for a range of Waveshare hardware. What is missing from the
vendor repo is only the board glue between them.

**Not yet verified:** whether `xiaozhi-esp32` upstream carries a board config for the
**V2** revision specifically (CO5300 + CST820). The V1/V2 split is recent — shipments
switched 2026-05-30 — so this needs checking before relying on it.

## Sources

- `ref/firmware/factory-backup-16MB-20260905.bin` — `strings`, and `gen_esp32part.py`
  against offset `0x8000`
- `ref/demo/ESP32-S3-Touch-AMOLED-1.8/` at `7ab8f95` — `docs/FIRMWARE.md`,
  `docs/EXAMPLES_GUIDE.md`, `examples/`, `Firmware/`
- [`bringup-20260905.md`](bringup-20260905.md) — the original dump and the V1/V2
  determination
