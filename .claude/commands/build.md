# Build an ESP-IDF project for ESP32-S3-Touch-AMOLED-1.8

Activate ESP-IDF and build out-of-tree. Usage: `/build <project_path>`

`<project_path>` is anything with a top-level `CMakeLists.txt` — a `projects/<name>/`
folder in this repo, or a vendor example under
`ref/demo/ESP32-S3-Touch-AMOLED-1.8/examples/esp-idf/<name>/`.

## Steps

1. Activate IDF (v5.5.3; the BSP requires `>=5.5,<6.1`):
   ```zsh
   . ~/esp/esp-idf/export.sh 2>/dev/null
   ```

2. Build directory **outside** the project, so vendor clones and the repo stay clean:
   ```zsh
   BUILD=/tmp/ws-amoled-build/$(basename <project_path>)
   ```

3. Build:
   ```zsh
   idf.py -C <project_path> -B "$BUILD" build 2>&1 | tail -25
   ```

4. With a credential overlay (see CLAUDE.md "Credential pattern"):
   ```zsh
   idf.py -C <project_path> -B "$BUILD" \
     -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
   ```

## Every new project needs these in `sdkconfig.defaults`

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

The last line matters: without it the S3 boots at **160 MHz**. The vendor's own
`sdkconfig.defaults` omits it, and our first bringup boot logged
`cpu freq: 160000000 Hz` as a result.

## Known failure recovery

| Symptom | Cause | Fix |
|---|---|---|
| Changes to `sdkconfig.defaults` have no effect | `sdkconfig` was already generated and wins | `rm <project>/sdkconfig` and rebuild |
| `IDF_TARGET not set`, or Xtensa/RISC-V mismatch | Target defaulted to `esp32` | `idf.py -C <path> set-target esp32s3` (**not** `esp32c6` — that's the sibling C6 repo) |
| `The "path" field in the manifest file ...` points at a stranger's home dir | A vendor-shipped `dependencies.lock` has their build machine's `IDF_PATH` baked in | `rm <project>/dependencies.lock` and rebuild |
| BSP header not found / `bsp/esp-bsp.h` missing | `main/idf_component.yml` lacks the BSP | Add `waveshare/esp32_s3_touch_amoled_1_8: {version: "^2.0.3", public: true}` |
| Component resolution fails on IDF 6.x | BSP declares `idf: ">=5.5,<6.1"` | Use IDF 5.5.x |
| `managed_components/` appears inside a vendor clone | `idf.py` downloads deps into the project dir | Expected; it is gitignored |
