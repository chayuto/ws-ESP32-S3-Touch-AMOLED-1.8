---
name: new-project
description: Scaffold a new ESP-IDF project in this workspace for the Waveshare ESP32-S3-Touch-AMOLED-1.8. Use when starting any new firmware, demo, or experiment on this board, so the project inherits the verified board config (240 MHz, octal PSRAM, custom partition table, BSP pin) instead of a vendor example's defaults.
---

# Starting a new project on this board

Always copy `projects/01_project_template`. Never start from a vendor example in
`ref/demo/` — their `sdkconfig.defaults` omits the CPU frequency (you silently boot at
160 MHz) and the partition table (an LVGL app overflows the default 1 MB app partition).

## Scaffold

```zsh
cp -R projects/01_project_template projects/<name>
cd projects/<name>
rm -rf sdkconfig sdkconfig.old dependencies.lock managed_components build
sed -i '' 's/project(01_project_template)/project(<name>)/' CMakeLists.txt
```

Deleting the generated files matters. `managed_components/` and `dependencies.lock` will
be regenerated from `main/idf_component.yml` on the first build, and a stale `sdkconfig`
would override anything you change in `sdkconfig.defaults`.

Naming: `<NN>_<short_snake_case>` matching the template's own `01_project_template`, so
the directory listing reads in the order things were built.

## What the template already gives you

- `bsp_display_start()` — panel, touch and LVGL up, with `esp_lvgl_port` running its task
- `bsp_display_brightness_set()` — there is no backlight GPIO; this writes a controller register
- A tap-counting LVGL screen, so touch is proven end to end on first flash
- A 10-second heap heartbeat printing internal and PSRAM free — watch it for leaks
- The `i2c.master` pull-up warning muted across bring-up and restored afterwards

Strip what you do not need, but keep the heartbeat until the project is stable; a drifting
number is the cheapest leak detector available.

## Before the first build

- **`sdkconfig.defaults`** is the committed baseline: target, QIO/16 MB, octal PSRAM
  @ 80 MHz, **`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`**, USB-Serial/JTAG console, 1 ms
  FreeRTOS tick, custom partition table, Montserrat 14/20/28. Add to it; do not remove
  lines without a stated reason.
- **`partitions.csv`** — `factory` 4 MB, `storage` 4 MB SPIFFS. Resize if the app needs it.
- **`main/idf_component.yml`** — `waveshare/esp32_s3_touch_amoled_1_8: ^2.0.3`, public.
  That single dependency pulls the CO5300 panel driver, the touch driver, LVGL 9 and
  `esp_lvgl_port`.
- **Secrets** never go in `sdkconfig.defaults`. Put Wi-Fi and API credentials in
  `projects/<name>/sdkconfig.defaults.local` (gitignored) and layer them at build time:

  ```zsh
  idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
  ```

## Build, flash, confirm

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> build
idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> -p /dev/cu.usbmodem3101 flash
```

Then capture serial to confirm it actually runs — see the `serial-capture` skill. Check
`cpu freq: 240000000 Hz` in the boot log every time; it is the config mistake this board
makes easiest.

## Log for the agent

Follow the `agentic-logging` skill from the first line: the template already compiles
DEBUG in and keeps INFO as the default; raise every tag you define to DEBUG in
`app_main`, add a 10-second heartbeat with real numbers, and log every decision the
code makes, rejections included. Copy `sdlog.c`, `devcmd.c`, `timesync.c` and
`pcf85063.c` from `02_word_book_en` for the SD flight recorder, serial commands and a
real clock.

## Finish the project properly

- Give it a `README.md` with what it does, how to build it, and the **verified serial
  output** pasted in. Both existing projects do this, and it is what makes a result
  reproducible six months later.
- If bring-up taught you something about the hardware, put it in `CLAUDE.md` — that file
  is the board's authoritative record — and write the lab note in `docs/research/`.
- Update the Projects table in `CLAUDE.md` and `README.md`.
