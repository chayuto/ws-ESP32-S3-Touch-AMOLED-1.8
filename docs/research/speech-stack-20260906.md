# Speech on the ESP32-S3 — what runs, what doesn't, and one loose thread

**Date:** 2026-09-06
**Board:** ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM, 240 MHz, 128-bit SIMD, **no NN accelerator**
**Component:** `espressif/esp-sr` 2.5.3 (`efa8d907c6d457cd0f99dae6c6b493412d3078d4`)

## Goal

Answer a direct question — is there a better speech model for this hardware than the one
`02_word_book_en` already runs, and can this board do open-vocabulary speech-to-text.

Short answers: **no**, and **not on the chip**. The detail is below, along with one
finding that has not been tested and could change the second answer.

## The full esp-sr 2.5.3 inventory

### Speech command recognition — MultiNet

13 model directories. The vendor README caps MultiNet at **300 commands** and lists
`mn5q8`, `mn6`, `mn7` as the ESP32-S3 options in both languages.

| English (S3) | size | | Chinese (S3) | size |
| --- | --- | --- | --- | --- |
| `mn5q8_en` | 2.1 MB | | `mn4q8_cn` | 888 KB |
| **`mn7_en`** | **2.6 MB** | | `mn4_cn` / `mn4cn` | 1.7 MB |
| `mn6_en` | 3.6 MB | | `mn5q8_cn` | 2.1 MB |
| | | | `mn7_cn` / `mn7_cn_ac` | 2.6 MB |
| | | | `mn3_cn` | 2.8 MB |
| | | | `mn6_cn` / `mn6_cn_ac` | 3.5 MB |

`_MODEL_INFO_` records `MN7_v2_english_8_0.9_0.90` and `MN6_v11_english_8_0.9_0.90`.
The shared `fst` (trie language model) is 12 KB.

**That is the entire English menu: three models.** `02_word_book_en` runs the newest.

### Wake word — WakeNet

80+ directories, of which ~30 are English. Verified from each `_MODEL_INFO_`:

| model | phrase | | model | phrase |
| --- | --- | --- | --- | --- |
| `wn9_hiesp` / `wn9s_hiesp` | Hi, ESP | | `wn9_jarvis_tts` | Jarvis |
| `wn9_alexa` / `wn9s_alexa` | Alexa | | `wn9_mycroft_tts` | Mycroft |
| `wn9_computer_tts` | Computer | | `wn9_sophia_tts` | Sophia |
| `wn9_heywillow_tts` | Hey, Willow | | `wn9_himfive` | Hi, M Five |
| `wn9_heykira_tts3` | Hey Kira | | `wn9_hijason_tts2` | Hi, Jason |
| `wn10_mosaico` | Mosaico | | `wn10_heyhermes` | Hey, Hermes |
| `wn10_heynova` | Hey, Nova | | `wn9l_heygigi` | Heygigi |

Also French (`wn9l_fr_bonjouresp_tts3`) and Japanese (`wn9l_ja_konnichihaesp_tts3`).

Sizes vary by more than 10×, and the suffix is the lever:

| | size |
| --- | --- |
| `wn9s_hiesp` (**s** = small) | 132 KB |
| `wn9_hiesp`, `wn9_alexa` | 292 KB |
| `wn10_heynova` | 992 KB |
| `wn10_mosaico` | 1.5 MB |
| `wn8_hiesp` | 1.6 MB |

**Trap:** `wn9_customword` is *not* a blank for your own wake word — its `_MODEL_INFO_`
reads `WakeNet9_v1h24_小爱同学_3_0.620_0.627`. It is a Chinese base model used as a
training template. A genuine custom wake word means running Espressif's TTS training
pipeline, not editing a file.

Also note the wn10 family uses a different on-disk format — split `_MODEL_INFO_p1`/`p2`
and `wn10_data_p1`/`p2` — rather than the single `_MODEL_INFO_` the wn8/wn9 loaders use.

### VAD and noise suppression

`vadnet1_medium` 288 KB · `nsnet1` 816 KB · `nsnet2` 340 KB.

### Text to speech — Chinese only

`esp-tts/esp_tts_chinese`, voice `xiaole`. **There is no Espressif English TTS.** English
speech output means recorded clips, which is what `02_word_book_en` does. Worth knowing
before turning sound on.

## What 02 actually costs, measured on hardware

From the boot log and the built artefacts:

| | |
| --- | --- |
| `srmodels.bin` | 3,049,158 B in a 6 MB partition (**3.1 MB spare**) |
| `mn7_en` internal RAM | −41,628 B → **72,299 B free** |
| `mn7_en` PSRAM | −3,413,508 B → 4,137,028 B free |

**Internal RAM is the ceiling, not flash and not PSRAM.** 72 KB is the number that
governs whether anything else fits alongside.

Currently enabled: `CONFIG_SR_MN_EN_MULTINET7_QUANT`, `CONFIG_SR_VADN_VADNET1_MEDIUM`,
`CONFIG_SR_NSN_WEBRTC`, `CONFIG_MODEL_IN_FLASH`. The AFE reports `vad_init: true` and
**`wakenet_init`, `aec_init`, `ns_init`, `agc_init`, `se_init` all false** — three
shipped, unused levers (WakeNet gating, NSNet, AGC), each costing internal RAM from that
72 KB.

## Finding: open-vocabulary ASR does not fit, and cannot be made to

| model | weights | vs. 8 MB PSRAM |
| --- | --- | --- |
| Whisper tiny, int8/q5 | ~30–40 MB | 4–5× over, before activations |
| Vosk small en-us | ~40 MB | 5× over |
| sherpa-onnx streaming zipformer | tens of MB | over |

Memory is the smaller obstacle. A 240 MHz Xtensa LX7 with 128-bit SIMD and no NN
accelerator runs these many times slower than real time. No quantisation closes a 5×
memory gap and a large compute gap simultaneously.

This is corroborated by the vendor: the factory firmware ships a wake word and streams
everything else to a server — see
[`factory-firmware-20260906.md`](factory-firmware-20260906.md).

## The loose thread: `raw_string` — UNTESTED

`esp_mn_results_t` in `include/esp32s3/esp_mn_iface.h` returns more than command IDs:

```c
char string[256];        // recognized string with commands graph
char raw_string[256];    // recognized string WITHOUT commands graph
```

MultiNet decodes in phonemes. 02's own boot log prints its loaded vocabulary that way:

```
Command 2: BeL      <- BALL
Command 6: KaT      <- CAT
Command 7: DeG      <- DOG
```

`string` is the decode snapped to the command list. `raw_string` is documented as the
decode *without* it. The same header names three decoders, and 02 runs the most
constrained (its log prints `search method: 2`):

```c
ESP_MN_GREEDY_SEARCH = 0,
ESP_MN_BEAM_SEARCH = 1,
ESP_MN_BEAM_SEARCH_WITH_FST = 2,
```

**If `raw_string` carries a free-running phoneme sequence for arbitrary speech**, then
phonemes plus a pronunciation dictionary on the SD card is an offline transcript on a
board that is not supposed to manage one. That would sidestep the 300-command cap
entirely.

**Reasons to expect disappointment:**

- No public API sets the search method; it is chosen internally when commands load.
- MN7 is trained on short commands with a 5.0 s timeout, not continuous speech.
- Phoneme→word without a language model is error-prone even given a clean phoneme
  string, and this one will not be clean.
- `raw_string` may simply be empty in FST mode.

**Cost to settle it:** one `ESP_LOGI` of `res->raw_string` beside the existing result
handling in `02_word_book_en/main/recognizer.c`, one flash, then say things outside the
20-word list and read the serial. Ten minutes. Until someone does this, the second
question at the top of this note is answered "no" on theory alone.

## Finding: rendering the text is a separate problem, and Chinese costs a font

Relevant to anything that displays recognised speech.

**English is free** — LVGL ships Montserrat at 8–48 px and handles UTF-8 natively.

**Chinese is not.** LVGL does bundle a CJK face, and it is a demo set rather than a
reading font. Counted from the `--symbols` line of the generator options embedded in
`lv_font_source_han_sans_sc_16_cjk.c`:

| | |
| --- | --- |
| total symbols | 1,293 |
| **CJK ideographs** | **1,118** |
| kana | 150 |

Source files are 1.02 MB (14 px) and 1.22 MB (16 px), 4 bpp, from
`SourceHanSansSC-Normal.otf` plus a FontAwesome range.

Running text wants ~3,000–3,500 characters for ~99 % coverage. Building one with
`lv_font_conv` from Noto Sans SC costs roughly 0.6 MB at 16 px and ~1.3 MB at 24 px.

**It does not have to live in flash.** LVGL 9 ships `binfont_loader` and `font_manager`,
so the `.bin` can sit on the SD card and load at runtime — and every project here already
mounts the card at boot.

## Conclusions

1. `mn7_en` is the best on-device English model available. There is no upgrade to buy.
2. The only cheap model-side experiment is **A/B `mn6_en` against `mn7_en`** on identical
   clips. 02's short single-syllable words are the hard case — `dog` 0.321 and `ball`
   0.561 against `cat` 0.924, with a phantom `CAR` at 0.138 — and newer is not
   automatically better on 400 ms utterances. Both fit the partition, one at a time.
3. Three shipped levers are unused: **WakeNet gating** (the real fix for false positives,
   but wrong for a toddler's word book and right for anything that moves), **NSNet**, and
   **AGC**. All three spend internal RAM from 72 KB.
4. Open-vocabulary speech-to-text has **no proven on-chip path**. Streaming it off the
   board is the only demonstrated approach, and it is **ruled out on product grounds**
   for `05_dictation`, which must run offline on battery. See
   [`../design/05_dictation.md`](../design/05_dictation.md).
5. That makes `raw_string` — and loading syllables rather than words as commands — not a
   curiosity but **the critical path**. Both are untested. Until someone runs the two
   experiments in that note, offline dictation on this board is unproven in either
   direction.

## Sources

- `projects/02_word_book_en/managed_components/espressif__esp-sr/` 2.5.3 — `model/`,
  `README.md`, `include/esp32s3/esp_mn_iface.h`, `Kconfig.projbuild`, `_MODEL_INFO_` files
- `projects/02_word_book_en/` — boot log memory figures, `sdkconfig.defaults`,
  `srmodels.bin`
- `managed_components/lvgl__lvgl/src/font/lv_font_source_han_sans_sc_16_cjk.c`
- [`factory-firmware-20260906.md`](factory-firmware-20260906.md)
