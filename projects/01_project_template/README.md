# 01_project_template — copy this to start a project

The starting point for anything on this board. It brings up the whole display stack —
CO5300 QSPI panel, CST820 touch, LVGL 9.5 — and draws a screen you can tap.

Every touch is also logged to the serial console, so the template is verifiable without
looking at the panel.

## Use it

```zsh
cp -r projects/01_project_template projects/<your-project>
# edit the project() name in projects/<your-project>/CMakeLists.txt
# replace build_ui() in main/app_main.c
```

`CMakeLists.txt`, `main/idf_component.yml`, `partitions.csv` and `sdkconfig.defaults` are
board-correct as they stand.

## Build, flash, read

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/01_project_template -B /tmp/ws-amoled-build/01_project_template build
idf.py -C projects/01_project_template -B /tmp/ws-amoled-build/01_project_template \
       -p /dev/cu.usbmodem3101 flash
```

Then capture serial with the recipe in `.claude/commands/monitor.md` — `idf.py monitor`
silently does nothing in a non-TTY shell.

## Verified output (this unit, 2026-09-05)

```
I (453) cpu_start: cpu freq: 240000000 Hz
I (453) app_init: Project name:     01_project_template
I (459) app: 01_project_template starting
I (459) LVGL: Starting LVGL task
I (459) ESP32-S3-Touch-AMOLED-1.8: Initialize SPI bus
I (460) co5300: version: 2.1.0
I (460) co5300_spi: LCD panel create success, version: 2.1.0
W (541) co5300_spi: The 3Ah command has been used and will be overwritten by external initialization sequence
I (666) ESP32-S3-Touch-AMOLED-1.8: Touch CST816S 0x15 found
I (668) CST816S: IC id: 183
I (670) app: display up: 368x448, touch indev registered
I (710) app: UI drawn; tap the panel to see touch events here
I (710) app: alive: taps=0 internal=204751 psram=8237204
```

Tapping the panel adds lines like `I (…) app: touch #1 at x=184 y=224`.

The `3Ah command` warning from `co5300_spi` is expected: the BSP passes its own init
sequence, which overrides the driver's default pixel-format command.

## What this template establishes

- **`bsp_display_start()` is the whole bring-up.** One call gives panel, touch and the
  LVGL port. It calls `bsp_i2c_init()` internally.
- **The TCA9554 is not involved.** This template never calls `bsp_io_expander_init()` and
  the display and touch both work. Do not copy the C6 sibling board's rule that the
  expander gates those rails.
- **Draw buffers live in PSRAM** — the BSP sets `buff_spiram = true`. Internal heap after
  bring-up is ~205–243 KB; PSRAM ~8.24 MB.
- **An LVGL app does not fit the default 1 MB app partition**, hence `partitions.csv`
  (4 MB `factory`, plus a 4 MB `storage` SPIFFS using the label `bsp_spiffs_mount()` expects).

## Notes on the code

- `build_ui()` is the only function you should need to replace.
- Any LVGL call from outside the LVGL task must be wrapped in
  `bsp_display_lock()` / `bsp_display_unlock()`. The event callback does not need the
  lock — it already runs in the LVGL task.
- `bsp_i2c_init()` logs a pull-up warning on this board that is not a real fault. The
  template mutes the `i2c.master` tag across bring-up so a genuine error is not lost in
  it, then restores the previous level.
