# CLAUDE.md — ws-ESP32-S3-Touch-AMOLED-1.8

Workspace for the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (SKU 29957; `-EN` variant is 29958).

A 1.8" 368×448 QSPI AMOLED touch board built on the ESP32-S3R8, with AXP2101 PMU,
PCF85063 RTC, QMI8658 IMU, ES8311 audio, TCA9554 I/O expander, and a microSD slot.

Everything below was checked against this physical unit. Where a claim is inferred
rather than observed, it says so.

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
- As-shipped 16 MB flash image backed up to `ref/firmware/factory-backup-16MB-20260905.bin`

## Board Facts (verified from boot logs, not the datasheet)

- **Chip:** ESP32-S3 (QFN56) rev **v0.2**, efuse block rev v1.4, dual-core Xtensa LX7
- **CPU:** 240 MHz — **only if you set it**; the IDF default is 160 MHz (see Critical Rules)
- **PSRAM:** 8 MB octal, in-package (AP Memory gen-3, 64 Mbit, 80 MHz, 3V, 10-cycle
  fixed read latency). Boot log: `Adding pool of 8192K of PSRAM memory to heap allocator`.
- **Flash:** 16 MB external NOR, **QIO @ 80 MHz**, 3.3 V (eFuse says quad, 4 data lines)
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
| LCD RST / backlight / touch RST | **none** — all three are `GPIO_NUM_NC` |
| Touch INT | 21 |
| SD card CMD / CLK / D0 (SDMMC 1-bit) | 1 / 2 / 3 |
| USB D- / D+ | 19 / 20 |

Free for application use: whatever the board does not claim above. Check the schematic
(`ref/schematic/`) before committing a pin — several are brought out to the FPC/pad
positions rather than being truly unused.

## I²C Bus — what actually answers

Single shared bus, **SDA = GPIO 15, SCL = GPIO 14**, 400 kHz by default
(`CONFIG_BSP_I2C_FAST_MODE=y`). Live scan from this unit:

| Address | Device | Identity check |
|---|---|---|
| `0x15` | CST820 capacitive touch (**V2**; a V1 board answers at `0x38` instead) | driver logs `IC id: 183` |
| `0x18` | ES8311 audio codec | |
| `0x20` | TCA9554 I/O expander | |
| `0x34` | AXP2101 PMU | reg `0x03` `CHIP_ID` = `0x4a` |
| `0x51` | PCF85063 RTC | |
| `0x6b` | QMI8658 6-axis IMU | reg `0x00` `WHO_AM_I` = `0x05` |

The BSP logs `Please check pull-up resistances...` on init. That warning is expected —
the board has hardware pull-ups and every device above still enumerates. To silence it
across bring-up, `esp_log_level_set("i2c.master", ESP_LOG_ERROR)` before
`bsp_display_start()` and restore after; `01_project_template` does exactly this.

## Repo Layout

```
ws-ESP32-S3-Touch-AMOLED-1.8/
├── CLAUDE.md                 # this file — authoritative board notes
├── README.md
├── .claude/
│   ├── commands/             # /build /flash /monitor /hardware-specs /peripherals /restore-factory
│   └── skills/               # new-project, serial-capture, flash, agentic-logging
├── .gitignore                # excludes /ref/, build/, sdkconfig, managed_components/
├── docs/
│   ├── research/             # lab notes and surveys (bringup-20260905.md is the founding one)
│   └── internal/             # working notes (gitignored)
├── components/
│   └── amoled_photo/         # shared: JPEG/raw photo → 368×448 RGB565 card, scale-to-fit, cache
├── projects/
│   ├── bringup-01/           # first-boot validation: chip report + I²C scan + revision detect
│   └── 01_project_template/  # copy this to start anything: BSP + display + touch + LVGL
└── ref/                      # vendor material (gitignored, ~650 MB)
    ├── datasheets/           # ESP32-S3, AXP2101, QMI8658C, PCF85063A, ES8311, CO5300, SH8601A0, FT3168
    ├── schematic/            # ESP32-S3-Touch-AMOLED-1.8-schematic.pdf
    ├── wiki/                 # offline HTML snapshot of docs.waveshare.com
    ├── firmware/             # full 16 MB backup of the as-shipped flash
    └── demo/ESP32-S3-Touch-AMOLED-1.8/   # official vendor repo clone
```

`ref/` is **not** in git — it is reproducible from the URLs listed in `ref/README.md`.

## Projects

| Project | What it is | Hardware-verified |
|---|---|---|
| `bringup-01` | Chip report, I²C census, V1/V2 revision detection. No display. | yes — full output in its README |
| `01_project_template` | The scaffold. BSP init, display, touch, LVGL 9 screen with a tap counter, heap heartbeat. | yes — display, touch registration, 240 MHz, stable heap |
| `02_word_book_en` | First application: voice-triggered picture book on ESP-SR. Built in milestones; see `docs/design/02_word_book_en.md`. | M0–M2 and M4 done 2026-09-05: audio, continuous MultiNet7 (no wake word), word → card → chime loop, dimming, silent tap-to-wake. Live adult speech 5/5 and the chime confirmed by a person. M3 on the child, the SD/photo path and a tap remain |

## ESP-IDF Environment

- **Version required:** ≥ v5.5, < v6.1 (BSP constraint). We use **v5.5.3** at `~/esp/esp-idf`.
- **Activate:** `. ~/esp/esp-idf/export.sh` (source it; Claude Code prompts for the `.` builtin once per session)
- **Python venv:** `~/.espressif/python_env/idf5.5_py3.14_env/bin/python` — has pyserial; system python does not.

## Build & Flash

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> build
idf.py -C projects/<name> -B /tmp/ws-amoled-build/<name> -p /dev/cu.usbmodem3101 flash
```

Out-of-tree build dirs (`-B /tmp/...`) keep vendor clones and the repo clean.
`idf.py monitor` does **not** work in non-TTY shells — see `/monitor` for the pyserial
recipe and the macOS DTR/RTS trap it works around.

Verify a flash without reflashing:

```zsh
cd /tmp/ws-amoled-build/<name>
esptool --chip esp32s3 -p /dev/cu.usbmodem3101 -b 921600 verify_flash @flash_args
```

## Shared components (`components/`)

Code more than one project wants lives here, not copied between projects. A project
opts in with `set(EXTRA_COMPONENT_DIRS ../../components)` in its top `CMakeLists.txt`
before `project.cmake`, then `REQUIRES <name>` in `main/CMakeLists.txt` or just
`#include` (main depends on everything by default).

| Component | What |
|---|---|
| `amoled_photo` | `photo_load()` / `photo_from_jpeg()`: JPEG or raw → 368×448 RGB565 card, scaled to fit with tinted bands, cached beside the JPEG. Host twin in `02_word_book_en/tools/make_book.py`. |

## Starting a New Project

Copy the template; do not start from a vendor example.

```zsh
cp -R projects/01_project_template projects/<name>
cd projects/<name>
rm -rf sdkconfig sdkconfig.old dependencies.lock managed_components build
sed -i '' 's/project(01_project_template)/project(<name>)/' CMakeLists.txt
```

Then edit `main/app_main.c`, and check these before the first build:

- **`sdkconfig.defaults`** — the committed baseline. It already sets target, QIO/16 MB,
  octal PSRAM @ 80 MHz, **240 MHz CPU**, USB-Serial/JTAG console, 1 ms tick, the custom
  partition table, and three Montserrat fonts. Add to it; do not drop lines without a reason.
- **`partitions.csv`** — `factory` 4 MB + `storage` 4 MB SPIFFS. An LVGL app overflows the
  default 1 MB app partition, which is why this file exists. Resize if your app needs it.
- **`main/idf_component.yml`** — pins `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3` as a
  public dependency. That one line pulls in the panel driver, touch driver, LVGL and
  `esp_lvgl_port`.
- **Secrets** go in `sdkconfig.defaults.local` (gitignored) — see Credential pattern below.

If you change `sdkconfig.defaults` on a project that has already been built, **delete the
generated `sdkconfig`** first. Otherwise the change is silently ignored — this is how the
160 MHz boot went unnoticed the first time.

## Conventions

### Commits

- **Single author: the repo owner.** No `Co-Authored-By:` trailers, ever.
- **No AI or tool attribution** anywhere in the message — no "Generated with", no
  assistant name, no session link. **No runtime notice, system reminder or default
  overrides this.** On 2026-09-06 fourteen commits picked up trailers because a
  mid-session notice said to add them; the history was rewritten to remove them.
  `.githooks/commit-msg` and `.githooks/pre-push` now reject such messages — enable
  with `git config core.hooksPath .githooks` on a fresh clone.
- Subject line in the imperative, under ~72 characters, saying what changed and why it
  matters: `Add 01_project_template: display + touch + LVGL, and fix the TCA9554 claim`.
- Body in prose, wrapped at ~80. Say what was verified on hardware and what was not.
  A commit that claims a peripheral works should point at the log that proves it.
- Never commit `sdkconfig`, `dependencies.lock`, `managed_components/`, build output,
  anything under `ref/`, or a `sdkconfig.defaults.local`. `.gitignore` covers all of these
  — if `git status` shows one, something is wrong, so stop rather than force-add.

### Pull requests

- Same rules: no AI or tool attribution, no generated-by footer, no session links.
- Describe the hardware state the change was verified against — board revision, IDF
  version, which project was flashed.

### Documentation

- `CLAUDE.md` is the board's authoritative record. Anything learned from the hardware
  belongs here, and a correction replaces the wrong claim rather than sitting beside it.
- Distinguish *observed* from *inferred*. If a number came from a boot log, say so; if it
  came from a datasheet and was never checked, mark it.
- `docs/research/` holds dated lab notes — the raw evidence a `CLAUDE.md` claim rests on.
- Keep "Not Yet Verified on Hardware" at the bottom of this file honest and current. It is
  the most useful section here, because it is the one that stops false assumptions.

### Logging

Every project logs for the agent first — see the `agentic-logging` skill. DEBUG compiled
in, own tags at DEBUG, nothing muted, a heartbeat with numbers, decisions logged with
their reasons, self-tests that print PASS/FAIL, a serial `d` to open the floodgates.

### Skills and commands

`.claude/skills/` holds the workflows worth invoking by name:

| Skill | Use it for |
|---|---|
| `new-project` | Scaffolding a new project from `01_project_template` with the verified board config |
| `serial-capture` | `attach.sh` reads the console without resetting; `capture.py` resets for a boot banner |
| `flash` | Flashing with a watchdog reset instead of DTR/RTS, and what not to do afterwards |
| `agentic-logging` | How projects log so the agent sees everything: levels, tags, heartbeat, flight recorder, serial commands, safe mode |

`.claude/commands/` holds the slash-command references: `/build`, `/flash`, `/monitor`,
`/hardware-specs`, `/peripherals`, `/restore-factory`.

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
- **The TCA9554 holds the display and touch reset lines — but its power-on default
  releases them.** The schematic text (2026-09-06) puts `LCD_RESET` on `EXIO0` and
  `TP_RESET` on `EXIO2`. The BSP never initialises the expander and `bsp_display_start()`
  still works, because a TCA9554 powers up with all pins as inputs (weak pull-up), so the
  resets float released. Drive them only if you need a real reset; otherwise leave the
  expander alone. (An earlier note here said the expander was "not in the display path";
  it is, passively.)
- **Bring the display up yourself if the project has Wi-Fi.** `bsp_display_start()`
  allocates LVGL's draw buffer with `MALLOC_CAP_DEFAULT` and ignores the DMA/PSRAM flags
  it is handed; under RAM pressure the buffer lands in PSRAM and every SPI flush then
  needs an internal DMA bounce buffer, which fails (`spi_master: Failed to allocate priv
  TX buffer`, screen frozen). Kconfig tweaks only moved the failure. What works:
  `lvgl_port_init` → `bsp_display_new` → `lvgl_port_add_disp` with `buff_dma = true` and a
  20-line buffer (14.7 KB, internal) → `bsp_touch_new` + `lvgl_port_add_touch` —
  `cards_display_start()` in `02_word_book_en`, ~40 lines. Verified: zero flush errors
  with Wi-Fi, HTTP and the recogniser all resident.
- **Wi-Fi station: `esp_wifi_set_ps(WIFI_PS_NONE)` if the board serves anything.** With
  power-save on (the default) the board joined, got an IP, answered ARP — and dropped
  every ping and TCP connect from the LAN. Off, everything answered at once.
- **MultiNet7's `destroy()` double-frees** (tlsf assert from `multinet7_quantized.c:346`
  with the heap verified clean beforehand, in either order relative to
  `esp_mn_commands_free()`). Build the engine once per boot and only stop its tasks.
- **A log hook must not use the caller's stack.** The SD flight-recorder hook runs on
  every task that logs; a 512-byte stack buffer overflowed the 2.3 KB system event task
  the moment DEBUG lines were compiled in. Static buffer under esp_log's lock, and
  `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`.
- **Touch LVGL only under the lock.** `bsp_display_lock(timeout_ms)` /
  `bsp_display_unlock()` around every LVGL call made outside an LVGL event callback.
  The LVGL task runs on its own; unlocked access from another task will corrupt it.
- **The AXP2101 owns the power rails.** Misconfiguring it can brown out the display or the
  card slot. Read `ref/datasheets/AXP2101.pdf` before changing any rail, and prefer the
  vendor example `90_axp2101_pmu` as the reference sequence.
- **ESP32-S3 is dual-core** — `xTaskCreatePinnedToCore()` is valid here, unlike on the
  single-core C6 sibling boards.
- **JPEG decode is software, via `esp_new_jpeg` (already a BSP dependency).** ~220 ms
  for 1 MP to RGB565 on this chip; the decoder scales 1/2–1/8 in-loop so a 12 MP photo
  fits PSRAM. Output must be 16-byte aligned. Verified in `02_word_book_en`.
- **PSRAM is 8 MB octal** — full framebuffers and LVGL buffers belong there.
  368×448×2 bytes = 330 KB per full-screen RGB565 buffer, which does not fit comfortably
  in internal RAM.
- **Never flash while a person is using the board.** The flash resets it; the screen
  goes dark for seven seconds and the self-test runs. Done once mid-use on 2026-09-06 and
  it looked like a fault. Ask for a "go" first.
- **One reset at a time. Never two host-driven resets within ten seconds.** Twice on
  2026-09-05 the board wedged — black screen, zero console bytes, ROM not answering
  esptool, USB still enumerated — each time right after `idf.py flash`'s `hard_reset`
  was followed within seconds by a pyserial open (which resets again through the
  USB-Serial-JTAG DTR/RTS emulation). **Flash with the `flash` skill** (`--after
  watchdog_reset`, no line toggling) **and look at the board with `attach.sh`**, which
  does not reset. `capture.py` only for the boot banner, and only once.
- **There is a battery, and unplugging USB is not a power cycle.** Measured 2026-09-06
  via the AXP2101: battery present, 4.12 V, 100 %, system 4.36 V on USB. The board ran
  92 minutes straight through an unplug and replug. Consequences: a "dark screen" on an
  unplugged board is a *running* board (dim, asleep, or a rail problem), and a wedge
  needs PWR, not the cable. `02_word_book_en` logs every rail and the battery state at
  boot, in the heartbeat, in a timed power record file, and on every VBUS change.
  **Recovery from a wedge: hold PWR 8–10 s, release, press once.** If the flashed image
  is crash-looping, power up **into download mode** instead — hold BOOT while pressing
  PWR, keep BOOT ~2 s — so nothing runs and esptool can connect. Every such recovery is
  a manual step for the person at the desk, which is why the rule above exists, and why
  `02_word_book_en` drops into a safe mode after three consecutive crashes.
- **Sleep must stop the recogniser, not pause it.** Measured 2026-09-06: with the front
  end paused but still running, 72 minutes asleep on battery took the gauge from 95 % to
  46 %, the same drain as in use. `02_word_book_en` now stops the tasks and frees the AFE
  on sleep and rebuilds it on wake. Same day: BOOT on a lit screen used to sleep the
  board, and a "black screen, no response to any button" report was the user's own
  presses; the card records showed it, the board had never reset. BOOT now only wakes,
  and every press is acknowledged on the status line.
- **Restoring the shipped firmware is possible** — see `/restore-factory`. Take a fresh
  backup before any flash that you cannot otherwise undo.

## Power Rails (AXP2101, read back 2026-09-06 on USB)

| Rail | Setting | Schematic net / use |
|---|---|---|
| DCDC1 | 3.3 V on | `VCC3V3`, the main rail |
| DCDC2, 3, 4 | on | DCDC3 ≈ 1.2 V (ESP32-S3 core per the schematic's power table) |
| DCDC5 | off | |
| ALDO1 | 3.3 V on | `VL1_3.3V` — the vendor sketches switch ALDO1/2 on at boot and off for sleep |
| ALDO2 | 3.3 V on | `VL2_3.3V` |
| ALDO3 | 3.0 V on | `VL3_3.3V` area, near `EXIO0`/`LCD_RESET` |
| ALDO4 | 1.8 V on | 1.8 V rail |
| BLDO1 | 1.2 V on | |
| BLDO2 | 2.8 V on | a panel-class voltage |
| CPUSLDO | 1.2 V | |
| DLDO1/2 | 0.5 V (unused) | |

`02_word_book_en/main/pmu.c` is the reader; it also sets ALDO1/ALDO2 to 3.3 V at boot as
the vendor does. Which rail, if any, drops on battery is **not yet known** — the power
record file catches it on the next unplug.

## Memory Budget (measured, not estimated)

| After boot | Internal free | PSRAM free |
|---|---|---|
| `bringup-01` — no display | 372,359 B | 8,385,100 B |
| `01_project_template` — panel + touch + LVGL, idle | 243,155 B | 8,237,204 B |

So the display stack costs roughly **129 KB internal and 149 KB PSRAM**. Internal RAM is
the scarce resource; anything large goes to PSRAM with `MALLOC_CAP_SPIRAM`. Both figures
above were flat across repeated 10-second heartbeats — a drifting number means a leak.

`02_word_book_en` runs with 56 KB internal free while listening and a 17 KB minimum in
maintenance mode (Wi-Fi); its full accounting — flash partitions and the 5 MB left
unallocated, what is in the 3.9 MB image, every internal-RAM and PSRAM user, card usage,
and what a feature costs — is `docs/design/02_word_book_en_memory.md`. Two rules from it:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` means a plain `malloc()` under 16 KB lands in
internal RAM, so allocate with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` on purpose; and
anything with a radio (Wi-Fi, BLE) is a mode that stops the recogniser, never a
background feature.

## Expected log lines (a healthy boot)

Do not chase these:

- `W (…) i2c.master: Please check pull-up resistances…` — hardware pull-ups exist; benign.
- `W (…) co5300_spi: The 3Ah command has been used and will be overwritten by external
  initialization sequence` — the BSP supplies its own init list including pixel format.

Anything else at `W` or above on a clean boot is worth reading.

## Credential pattern

`sdkconfig.defaults` is **committed** and must contain no secrets. Real Wi-Fi or API
credentials go in `projects/<name>/sdkconfig.defaults.local`, which is gitignored, and are
layered in at build time. (The home network's credentials are in the CAM sibling's
gitignored `projects/cry-detect-01/sdkconfig.defaults` under `CONFIG_EXAMPLE_WIFI_*`;
`02_word_book_en/sdkconfig.defaults.local` has them under `CONFIG_WORDBOOK_WIFI_*`.)

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
Vendor `sdkconfig.defaults` files are **not** a good baseline — they omit the CPU
frequency and the partition table. Use ours.

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

## Not Yet Verified on Hardware

Honest list, so nobody builds on an assumption:

- **Audio: verified end to end.** Mic → ESP-SR recognises live adult speech (five for
  five, `02_word_book_en`), speaker chime heard by a person. Mic gain 30 dB and volume 90
  are the working values.
- ~~Touch~~ — confirmed 2026-09-05: taps logged from the CST820 through LVGL in `02_word_book_en`.
- ~~microSD~~ — confirmed 2026-09-05: SanDisk 16 GB mounts in 56–224 ms, log file appends,
  hot removal and reinsertion handled live, JPEGs read and decoded from it, 330 KB cache
  files written back (~1.5 s each — SDMMC 1-bit writes are slow).
- **microSD, before that** — no card has been in the slot. `bsp_sdcard_mount()` fails cleanly without
  one (`sdmmc_init_ocr ... 0x107`, ~27–50 ms). `02_word_book_en` has the full read path
  written plus hot-insert, removal-while-running and a live vocabulary swap, all
  untested; a card with `tools/make_book.py --demo` output, inserted while running,
  tests the lot. There is **no card-detect pin** — presence is polled.
- ~~RTC~~ — PCF85063 verified 2026-09-05 in `02_word_book_en`: set from NTP, read back
  correctly on a later boot, keeps counting across reboots. CLKOUT is switched off at init.
- ~~Wi-Fi (STA)~~ — joins the home network in ~7 s and NTP syncs (`02_word_book_en`).
  Linking it costs ~55 KB of internal RAM even after `esp_wifi_deinit()` (166 → 110 KB
  free), so it is brought up once at boot, before the recogniser, and torn down.
- **IMU** — `WHO_AM_I` and address confirmed only; no readings taken.
- **AXP2101 rails** — `CHIP_ID` read only; no rail configured or measured.
- **Battery operation** — never run off the MX1.25 connector.
- **Wi-Fi as an AP.** Tried in `02_word_book_en` on 2026-09-05: the AP started, the
  phone side did not work out, and the feature was removed the same day. STA mode works
  (above).
- **What the TCA9554 lines actually drive.**
