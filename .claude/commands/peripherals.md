# Peripheral cookbook — ESP32-S3-Touch-AMOLED-1.8

Use the BSP component `waveshare/esp32_s3_touch_amoled_1_8` for anything nontrivial. It
hides the TCA9554 power-gating, the I²C bus setup, the QSPI panel sequence, and the
touch-controller auto-detect.

## Dependency line

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=5.5,<6.1"
  waveshare/esp32_s3_touch_amoled_1_8:
    version: "^2.0.3"
    public: true
```

## Init order — this order matters

```c
#include "bsp/esp-bsp.h"

bsp_i2c_init();            // GPIO 15/14, shared by all six I2C devices

// then, as needed:
lv_display_t *disp = bsp_display_start();     // panel + touch + LVGL port, all in one
bsp_display_brightness_init();
bsp_display_brightness_set(80);               // percent; writes a controller register

bsp_audio_init(NULL);
esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();

bsp_sdcard_mount();        // -> /sdcard, SDMMC 1-bit
bsp_spiffs_mount();        // -> /spiffs, partition label "storage"
```

`bsp_display_start()` calls `bsp_i2c_init()` itself, so the explicit call above is only
needed when you want the bus up before the display.

**The TCA9554 is not part of this sequence.** `bsp_io_expander_init()` exists but the BSP
never calls it, and `01_project_template` brings up panel and touch without it. Call it
only when you need the `EXIO0`–`EXIO7` lines. The C6 sibling board *does* gate display and
touch power through its expander; this board does not.

## Full BSP API

| Function | Notes |
|---|---|
| `bsp_i2c_init()` / `bsp_i2c_deinit()` | Shared bus; `bsp_i2c_get_handle()` returns the `i2c_master_bus_handle_t` |
| `bsp_io_expander_init()` | TCA9554; returns `esp_io_expander_handle_t` |
| `bsp_display_new()` | Raw panel + panel-IO handles, no LVGL |
| `bsp_display_start()` / `bsp_display_start_with_config()` | Panel + touch + LVGL port |
| `bsp_display_brightness_init()` / `bsp_display_brightness_set(pct)` | Controller register, not PWM |
| `bsp_display_backlight_on()` / `_off()` | Convenience wrappers over brightness |
| `bsp_display_lock(ms)` / `bsp_display_unlock()` | **Required** around any LVGL call from another task |
| `bsp_display_rotate(disp, rot)` | LVGL-level rotation |
| `bsp_display_get_input_dev()` | The LVGL touch indev |
| `bsp_touch_new()` | Touch handle without LVGL |
| `bsp_audio_init()`, `bsp_audio_codec_speaker_init()`, `bsp_audio_codec_microphone_init()` | ES8311 via `esp_codec_dev` |
| `bsp_sdcard_mount()` / `bsp_sdcard_unmount()` | `/sdcard` |
| `bsp_spiffs_mount()` / `bsp_spiffs_unmount()` | `/spiffs`, partition `storage` |

## Display notes

- 368×448 RGB565 = 330 KB per full frame. Allocate LVGL buffers from PSRAM.
- `CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH` is the default buffer mode;
  `CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR` exists if you see tearing.
- V2 drives a **CO5300**; V1 drives an SH8601. BSP v2.0.3 only ships the CO5300 path.
- The CST816S touch path applies a 16 px X gap — if touch coordinates are offset by
  exactly 16, that constant is why.

## Peripherals the BSP does *not* cover

`BSP_CAPS_IMU` is 0 and there is no PMU or RTC API. Talk to these directly on the shared
bus (`bsp_i2c_get_handle()`), or copy the vendor examples:

| Part | Address | Vendor example |
|---|---|---|
| AXP2101 PMU | `0x34` | `ref/demo/ESP32-S3-Touch-AMOLED-1.8/examples/esp-idf/90_axp2101_pmu` |
| PCF85063 RTC | `0x51` | `.../91_pcf85063_rtc` |
| QMI8658 IMU | `0x6b` | `.../92_qmi8658_imu` |

Minimal register read on the shared bus:

```c
i2c_master_dev_handle_t dev;
i2c_device_config_t cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x6B,          // QMI8658
    .scl_speed_hz    = 100000,
};
i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev);
uint8_t reg = 0x00, who = 0;
i2c_master_transmit_receive(dev, &reg, 1, &who, 1, 200);   // who == 0x05
i2c_master_bus_rm_device(dev);
```

## Task placement

Dual-core: pin Wi-Fi/lwIP work to core 0 and rendering or DSP to core 1
(`xTaskCreatePinnedToCore`). Do not copy the C6 sibling repo's "never pin" rule — that
exists because the C6 is single-core.
