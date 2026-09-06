# 02_word_book_en — memory and space

Where every byte goes, what is left, and what a new feature costs. Every figure here was
read from a build or from the running board on 2026-09-06 (commit `006c882`, build of
`aaa4b20` on the board), not estimated. Re-measure with the commands at the end whenever
something big changes; a number that drifts between heartbeats is a leak.

## The four budgets at a glance

| Budget | Total | Used | Free | Verdict |
|---|---|---|---|---|
| App partition (flash) | 4 MB | 3,897,264 B | 296,016 B (7 %) | **tight** — the partition table wastes 5 MB, see below |
| Model partition (flash) | 6 MB | 3,049,158 B | 2.95 MB | room for a wake-word model or a second language |
| Storage partition (flash) | 1 MB | nothing | 1 MB | unused; content lives on the card |
| Unallocated flash | — | — | 4.94 MB | free to claim |
| Internal RAM, static | 341,760 B | 258,903 B | 82,857 B | **the real ceiling** |
| Internal RAM, runtime heap | — | — | 56,595 B listening; 17–19 KB minimum in maintenance | every new byte must go to PSRAM |
| PSRAM | 8 MB | ~4.25 MB | 4,137,456 B | comfortable |
| Vocabulary | 400 phrases (engine) | 19 words | capped at 32 by a constant | confusability limits it first |
| SD card | card size | ~1 MB per 3 words + logs | effectively unlimited | logs are bounded by rotation |

## Flash

### The partition table as flashed

`projects/02_word_book_en/partitions.csv`, 16 MB chip:

| Name | Offset | Size | Holds | Used |
|---|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | Wi-Fi calibration, NVS | — |
| `phy_init` | 0xF000 | 4 KB | PHY data | — |
| `factory` | 0x10000 | 4 MB | the app image | 3,897,264 B (7 % free) |
| `model` | 0x410000 | 6 MB | `srmodels.bin`: MultiNet7 EN (quantised) + vadnet1 medium | 3,049,158 B |
| `storage` | 0xA10000 | 1 MB | nothing — no code opens it | 0 |
| *(unallocated)* | 0xB10000 – 0x1000000 | 4.94 MB | — | — |

The model partition is flashed by `flash_args` alongside the app; the models are read
from flash into PSRAM at boot (`CONFIG_MODEL_IN_FLASH=y`).

### What is in the 3.9 MB app image

`idf.py size` (2026-09-06): flash code 2,446,290 B, flash data 1,280,168 B. The heavy
components, from `idf.py size-components`:

| Archive | Total | What it is |
|---|---|---|
| `libespressif__esp-dl.a` | 836 KB | the neural-network runtime under MultiNet and the VAD |
| `libflite_g2p.a` | 610 KB (579 KB data) | English grapheme-to-phoneme: what lets a filename become a command |
| `libmain.a` | 419 KB | our code 29 KB + **embedded assets 290 KB** + the 72 px font 85 KB |
| `liblvgl__lvgl.a` | 374 KB | LVGL 9.5 |
| `libdl_lib.a` | 219 KB | more ESP-SR kernels |
| `libnet80211.a` + `liblwip.a` + `libwpa_supplicant.a` + `libpp.a` | 397 KB | Wi-Fi and TCP/IP (maintenance mode, NTP) |
| `libesp_audio_processor.a` | 86 KB | the AFE (audio front end) |
| `libesp_new_jpeg.a` | 71 KB | JPEG decode |
| `libmbedcrypto.a` | 62 KB | pulled in by Wi-Fi |

The embedded assets are the three synthesised self-test clips (209 KB), the built-in
`baby.jpg` (74 KB) and `maint.html` (6.6 KB). They are the cheapest flash to win back:
moving the clips and the photo to the card or the storage partition frees 283 KB without
touching the partition table.

### Growing the app partition — do this before the next big feature

Nothing on the chip needs the 5 MB that sits unallocated. The layout to move to when a
feature will not fit:

```
# Name,     Type, SubType, Offset,  Size,   Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 8M,
model,      data, spiffs,  ,        6M,
storage,    data, spiffs,  ,        1M,
```

That is `factory` 0x10000–0x810000, `model` 0x810000–0xE10000, `storage` to 0xF10000,
960 KB still spare. App free space goes from 296 KB to about 4.3 MB.

Cost: one full flash. `flash.sh` writes the bootloader, the partition table, the app and
`srmodels.bin`, and because the model partition moves, the 3 MB of models are rewritten
too — about a minute at 460800 baud. `nvs` keeps its offset, so Wi-Fi calibration
survives; the card is untouched. There is no OTA partition and none is planned: the
board is flashed over USB, and a single factory slot is what keeps 8 MB available.

## Internal RAM — the scarce one

The ESP32-S3 has 512 KB of SRAM; after the cache and the ROM's reservations the
application sees 341,760 B of DIRAM plus a fixed 16 KB IRAM block (vectors and the
hottest code, always 100 % used — that is normal).

### Static (from `idf.py size`)

| Section | Bytes | Notes |
|---|---|---|
| IRAM `.text` | 118,879 | code that must run from RAM: Wi-Fi, flash driver, ISRs, ESP-SR hot loops |
| `.bss` | 104,624 | zeroed globals: the book table (6.9 KB), sdlog's line buffer, Wi-Fi/LWIP state, ESP-SR |
| `.data` | 35,400 | initialised globals |
| **Total static** | **258,903 (75.8 %)** | leaves 82,857 B for every heap allocation that must be internal |

### Runtime heap

| Moment | Internal free | Source |
|---|---|---|
| Listening, idle card, card in | 56,595 B | heartbeat; flat to the byte over the 2026-09-06 10:07–11:07 watch |
| Asleep | 56,595 B | `i` query; nothing is freed or allocated by sleep |
| Maintenance mode (Wi-Fi + HTTP, recogniser stopped, AFE destroyed) | 47–51 KB free, **17–19 KB minimum** | `/api/metrics`, `internal_min` in state records |

The minimum is set by maintenance mode: Wi-Fi's buffers, LWIP, the HTTP server and its
6 KB stack all come out of internal RAM. That 17 KB is the number that decides whether a
feature is possible.

### Who uses internal RAM, and why it cannot move

| User | Bytes | Why internal |
|---|---|---|
| Task stacks: main 8192, `sr_detect` 8192, `sr_feed` 6144, `sdlog` 4096, `devcmd` 3072, system event 4096, LVGL port task, timer task, Wi-Fi and LWIP tasks in maintenance, `httpd` 6144 | ~45 KB + system | stacks must be internal unless `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`, which is off and stays off (flash ops from a PSRAM stack fault) |
| LVGL draw buffer | 14,720 (20 lines × 368 px × 2 B) | DMA to the QSPI panel — this is why `cards_display_start()` exists instead of the BSP's `bsp_display_start()` |
| I2S and SDMMC DMA descriptors and buffers | a few KB | DMA |
| AFE feed chunk (`recognizer.c`) and player chunk (2 KB) | ~2–4 KB | handed to the I2S driver |
| Wi-Fi (maintenance only): static RX 6, static TX 8, BA window 6; dynamic RX 16 and cache TX 16 go to PSRAM under `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | ~20 KB internal | radio DMA |
| LWIP: 8 sockets, TCP window and send buffer 5,760 B | a few KB | this diet is what let httpd fit; `LWIP_MAX_SOCKETS=6` broke it (httpd needs 3 for itself) |
| Anything under 16 KB from plain `malloc()` | — | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`: small default allocations land internal |

### Rules

1. **Allocate with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`** for anything that is not
   tiny and not DMA. Plain `malloc()` under 16 KB goes internal by configuration.
2. **No new static arrays in internal RAM.** A buffer that must exist for the life of the
   program is a PSRAM allocation at init (`sdlog.c` is the pattern: 64 KB + 2 × 32 KB
   rings, all PSRAM). The one static exception is sdlog's log-line buffer, which must not
   use the caller's stack because the hook runs on every task, including 2 KB ones.
3. **Wi-Fi and the recogniser never run together.** Maintenance mode stops the tasks and
   destroys the AFE before joining; the engine itself is kept because MultiNet7's
   `destroy()` double-frees. Anything else radio-shaped (BLE, HTTPS, OTA) gets the same
   treatment: a mode of its own, never a background feature.
4. **New tasks: smallest stack that passes `d` (DEBUG on every tag)**; the system event
   task overflowed at its default when sdlog's hook briefly used a stack buffer, and safe
   mode caught the loop.
5. **Check the heartbeat after every change.** `internal=` and `psram=` print every 10 s;
   both flat means no leak. `internal_min` in the state records is the worst moment since
   boot.

## PSRAM — 8 MB, about half spoken for

Free after boot: 4,137,456 B, flat. Used, about 4.25 MB:

| User | Bytes | Lifetime |
|---|---|---|
| ESP-SR: MultiNet7 EN, vadnet1, the AFE's buffers | ~3.2 MB (the remainder) | boot to power-off (AFE rebuilt around maintenance) |
| Two card buffers, RGB565 368 × 448 (`cards.c`) | 659,456 | permanent, double-buffered so a card can be built while the other shows |
| Display stack (LVGL objects, `esp_lvgl_port`) | ~149 KB | permanent (measured on the template: 149 KB PSRAM, 129 KB internal) |
| Log rings: main 64 KB, classifier 32 KB, state 32 KB | 131,072 | permanent |
| Photo decode: the JPEG file plus a decoded output up to 329,728 B | ≤ ~0.5 MB | while a card is built |
| WAV prompt (`player.c`), 1 s max | 32,000 | while playing |
| Maintenance request buffers, `words.json` text | 8 KB each | per request |

What fits: a decoded-card cache of about ten words (3.3 MB) if photo latency ever
matters, longer prompts, bigger rings, animation frames. A second MultiNet engine
(Chinese is 2.9 MB) would fit only just, and that belongs to project 3's own build.

## The SD card

Per word: the JPEG as dragged in (our shipped set averages 169 KB), a `.fit.rgb565`
cache of 329,728 B written the first time the word shows, and an optional 16 kHz mono
WAV prompt at 32 KB per second. Call it 1 MB per three words; a 32 GB card holds more
words than the engine allows.

Logs are bounded by rotation: each of the three files (`.log`, `.classifier.jsonl`,
`.state.jsonl`) rotates to `.1` at `CONFIG_WORDBOOK_LOG_MAX_KB` (4 MB), so together
they never exceed about 24 MB. Growth while awake is roughly 2 MB a day for the log
(INFO with 10 s heartbeats), 1.7 MB a day for state records (about 600 B every 30 s
plus events) and more for the classifier file (an `env` record every 5 s, a `det` per
utterance). Asleep, almost nothing: 158 B per `i` query during the watch, no
heartbeats, no records.

The card is also the fallback path's boundary: with no card the vocabulary is the 19
built-in words on text cards, the rings keep the last 64 KB of log in PSRAM, and
everything else runs unchanged.

## Vocabulary

| Limit | Value | Cost of raising it |
|---|---|---|
| `BOOK_MAX_WORDS` (`book.h`) | 32 | 216 B of `.bss` per word (`book_word_t`: text 24 + two 96-byte paths) — 64 words is 7 KB more |
| `ESP_MN_MAX_PHRASE_NUM` (esp-sr) | 400 | the engine's ceiling; each command's phoneme string is small and lives in PSRAM |
| Practical | 20–40 for a toddler | BALL↔CAR and THREE↔SEVEN already confuse; every added word adds neighbours |

## What a feature costs — worked examples

| Feature | App flash | Internal RAM | PSRAM | Fits today? |
|---|---|---|---|---|
| Another record, serial command, card layout (like the input record: +5 KB) | KB | none | none | yes, dozens of times |
| Another 72 px font | 85 KB | none | none | yes, at most two more |
| More built-in photos or clips | 74 KB each | none | none | prefer the card; otherwise grow the partition |
| A wake word (WakeNet9) | ~20 KB | +10–20 KB, on the tightest budget | +0.5 MB model | model partition yes; internal RAM needs measuring first |
| BLE (any profile) | 200–400 KB | 30–50 KB | some | **no**: grow the partition, and it can only run as a mode without the recogniser, like Wi-Fi |
| HTTPS, OTA, a cloud client | 200–400 KB | 40–50 KB for TLS | ~50 KB | **no** until the partition grows; maintenance mode only |
| Second-language MultiNet | ~0 | ~0 | +2.9 MB | model partition just fits; PSRAM tight; project 3's problem |
| Decoded-card cache, animations, longer audio | none | none | MBs available | yes |

The line is roughly: **under 250 KB of flash and zero internal RAM fits now; anything
bigger means the 8 MB partition first, and anything with a radio is a mode, not a
feature.**

## How to measure

```zsh
# static picture: sections, then the per-archive table
idf.py -C projects/02_word_book_en -B /tmp/ws-amoled-build/02_word_book_en size
idf.py -C projects/02_word_book_en -B /tmp/ws-amoled-build/02_word_book_en size-components

# the partition table the build will flash
idf.py -C projects/02_word_book_en -B /tmp/ws-amoled-build/02_word_book_en partition-table

# runtime, without resetting the board: heartbeat every 10 s while awake; 'i' answers asleep
.claude/skills/serial-capture/scripts/attach.sh 30
printf 'i' > /dev/cu.usbmodem3101      # with attach.sh running and -hupcl set

# worst moment since boot, and the maintenance-mode floor
jq -c 'select(.t=="state") | [.time, .event, .internal, .internal_min, .psram]' 02_word_book_en.state.jsonl
curl -s http://192.168.1.111/api/metrics   # in maintenance mode
```

A healthy board prints the same `internal=` and `psram=` on every heartbeat. The
2026-09-06 watch is the reference: 56,595 B internal and 4,137,456 B PSRAM at 32 s of
uptime and unchanged at 2,624 s.
