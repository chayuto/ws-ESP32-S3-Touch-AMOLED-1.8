# 02_word_book_en — a voice-triggered picture book for a toddler

*Design note, 2026-09-05. Status: proposed, not started.*

## What it is

A toddler says a word. The board shows the matching photo and says the word back.
That's the whole product. No menus, no wake word, no buttons a child has to understand.

From the child's side:

1. The screen shows a gentle idle picture (or the last photo).
2. Child says "dog".
3. Within half a second the family dog's photo fills the screen, a small chime plays, and
   a parent's recorded voice says "Dog!"
4. It stays there until the next word.

If a word is recognised but has no photo yet, the screen shows **the word itself**, large,
with the same chime. Two reasons this matters more than it looks:

- The vocabulary can be bigger than the photo set. Add photos as you take them; the book
  grows without the firmware knowing.
- M2 can be built and verified with *zero* photos — the text card proves the whole loop
  before any content work is done.

That's it. The thing it is practising is *saying the word and getting a response*. It is
not a quiz; it does not mark the child wrong. If the child mumbles something dog-shaped,
they get the dog.

**First version is English only.** That is not a preference — it's the only language the
on-device recogniser speaks (see *Recognition*). Multilingual is project 3, and the
architecture below is chosen so that project 3 replaces one module rather than starting
over.

## Why this board can do it

The ESP32-S3 is the one Espressif chip with a supported offline speech-command engine
(ESP-SR MultiNet). It needs PSRAM, a mic, and a spare core. We have all three:

| Need | This board |
|---|---|
| Recogniser: MultiNet7 needs **2,920 KB PSRAM**, 18 KB RAM, 11 ms per 32 ms audio frame | 8 MB PSRAM, ~8.2 MB free with the display stack up; second core is idle |
| Audio in: 16 kHz, 16-bit, mono | ES8311 ADC + onboard mic, via `bsp_audio_codec_microphone_init()` — the BSP defaults to 22,050 Hz but `bsp_audio_init()` takes a config, so 16 kHz is one struct |
| Audio out: chime + spoken word | ES8311 DAC + speaker amp (`BSP_POWER_AMP_IO` = GPIO 46); the vendor `12_i2s_codec` example proves playback |
| Photos: 368×448 RGB565 = 330 KB each | SD card slot (SDMMC 1-bit), photos streamed into a PSRAM buffer |
| Model storage: a `model` partition, docs suggest 6000K | 16 MB flash; the template currently splits 4 MB app / 4 MB storage, with 8 MB unassigned |

ESP-SR is a managed component (`espressif/esp-sr` 2.5.3, requires IDF ≥ 5.0 — we're on
5.5.3). Same pattern as the BSP: one line in `idf_component.yml`.

## Architecture

```
                 core 1                                   core 0
 ┌──────────────────────────────────┐        ┌──────────────────────────────────┐
 │  audio_task                      │        │  LVGL task (esp_lvgl_port)       │
 │  I2S rx 16 kHz mono              │        │  photo view, idle screen         │
 │  → AFE (NS, VAD, AGC)            │        │                                  │
 │  → MultiNet detect               │ word   │  book_task                       │
 │  → emit word_event(id)  ─────────┼──────▶ │  on word: load photo from SD,    │
 │                                  │ queue  │  swap into LVGL under lock,      │
 │  player: chime + prompt WAV      │◀───────┼  ask player to play prompt       │
 └──────────────────────────────────┘        └──────────────────────────────────┘
```

Three rules that keep this simple:

- **The recogniser is a module behind one event.** `word_event { id, confidence }` is the
  only thing that crosses the boundary. Project 3 swaps what's on the left; nothing on
  the right changes.
- **All audio on core 1, all UI on core 0.** MultiNet takes ~34% of a core continuously.
  Pinning it away from LVGL means a photo swap never stalls recognition and recognition
  never stutters the display.
- **Content lives on the SD card, not in flash.** Photos, voice prompts and the word list
  are files. Changing the book means editing a folder on a laptop, not reflashing. This
  also means the *same firmware* serves any family and, later, any language.

## Recognition

**Engine:** ESP-SR MultiNet7 English (`mn7_en`), with the AFE in single-mic mode.

**Vocabulary:** custom commands. MultiNet7 takes commands as phoneme strings produced by
`tool/multinet_g2p.py`, up to 200. We want **10–20**, not 200 — every extra word is
another thing for a mumbled syllable to be confused with.

Starter list, the words toddlers say first and the ones a family has photos of:
`mama`, `dada`, `dog`, `cat`, `ball`, `car`, `duck`, `baby`, `milk`, `apple`, `banana`,
`shoe`, `book`, `bird`, `fish`, `moon`. Finalise from *your* photos, not from a list.

**Tune for generosity, not accuracy.** This is the key design call and it runs opposite to
how the engine is normally used:

- A false positive costs nothing — a wrong photo appears, the child laughs, says it again.
- A false negative is the failure mode of every speech toy: the child repeats themselves
  into a screen that does nothing, and loses interest.

So: low detection threshold, and **multiple phoneme spellings per word**. MultiNet7 lets
one command ID have several phoneme strings. "dog" gets `DOG`, `DAW`, `DOH`, `GOG`. This
is how we meet toddler pronunciation halfway, and it's the part we tune on the real child.

**Wake word:** none. A toddler will not say "Hi ESP". The docs state MultiNet "must be
used with WakeNet"; the ESP-SR API separately exposes the AFE output and `detect()`, and
the Arduino `ESP_SR` wrapper runs command detection without a wake phrase. Running
MultiNet continuously is the first thing to verify on hardware, before anything else is
built. Fallback if it truly won't: use WakeNet with a long post-wake window and have the
*parent* say the wake word once.

**Honest limitation:** MultiNet is trained on adult speech. Toddler speech is higher, less
articulated, and inconsistent day to day. Nobody has published toddler accuracy numbers
for it. The generosity tuning above is the mitigation; the real answer is a test with the
actual child, which is milestone M3. If it's poor, project 3's approach (below) fixes it
in a way that also happens to be language-agnostic.

## Content on the SD card

```
/book/
  words.json          ← the vocabulary, one entry per word
  dog.rgb565          ← 368×448 raw, 330,624 bytes, displays with zero decode time
  dog.wav             ← parent's voice saying "dog", 16 kHz mono
  cat.rgb565
  cat.wav
  ...
  chime.wav
  idle.rgb565
```

`words.json` maps each word to its MultiNet command ID, its phoneme variants, and
*optionally* its photo and prompt. The firmware reads it at boot and builds the command
list from it — so **the vocabulary is data, not code.** A word with no photo shows as a
text card; a word with no prompt just gets the chime.

A host-side Python script (`tools/make_book.py`, Pillow) takes a folder of ordinary
photos + recordings and produces this layout: crop-to-fit, resize to 368×448, convert to
RGB565, resample audio to 16 kHz mono. Raw RGB565 rather than JPEG because the S3 has no
hardware JPEG decoder; 330 KB from SD to PSRAM is ~30 ms, a software JPEG decode of the
same frame is several times that, and SD space is not a constraint.

**Recorded voice, not TTS.** A parent's voice is what the child wants to hear, it needs
no TTS engine (2.2 MB of flash saved), and it makes the *output* side multilingual for
free — record the prompts in Thai and they're Thai. Only the recognition side is
language-locked.

## Budget

| Resource | Cost | Headroom |
|---|---|---|
| PSRAM: MultiNet7 2.9 MB + AFE ~0.3 MB + display stack 0.15 MB + 2 photo buffers 0.66 MB | ~4.0 MB | 4 MB spare of 8 |
| Internal RAM: recogniser ~50 KB + I2S DMA + audio buffers | ~100 KB | template idles with 243 KB free |
| Flash: app ~2 MB (LVGL + ESP-SR + drivers) in a 4 MB partition; `model` partition 4–6 MB for `wn9` + `mn7_en` + NS/VAD models | ~8–10 MB | 16 MB total; drop `storage` to 1 MB since content is on SD |
| CPU: MultiNet ~34% of core 1 + AFE; LVGL on core 0 | comfortable | — |

New `partitions.csv`: `nvs 24K / phy 4K / factory 4M / model 6M / storage 1M`.

## Risks, ranked

1. **Toddler speech vs an adult-trained model.** The whole product. Mitigated by
   generosity tuning; resolved only by M3.
2. ~~**MultiNet without WakeNet.**~~ Settled by M1: it works. The AFE warns once and runs.
   One thing M1 did find: on digital silence MultiNet emits a fixed junk detection (`CAR`
   at 0.135, VAD = silence). Gate on VAD, not on a high probability floor.
3. **Mic quality.** Onboard MEMS mic behind a case hole, child at unknown distance. AGC
   in the AFE helps. Check the mic gain path on the ES8311 early.
4. **Audio and SD sharing nothing** — I²S and SDMMC are on separate pins here, unlike
   some boards. Not a real risk; noted because it usually is.
5. **Speaker + mic in one small case** = feedback into the recogniser while a prompt
   plays. Simple fix: mute recognition while playing. No AEC needed. *Done.*
6. **The card is removable and a toddler will remove it.** No card-detect pin, so
   presence is polled; the vocabulary is kept when the card goes, only the files stop.
   Inserting a card swaps the vocabulary live. *Written; untested without a card.*

## What project 3 (multilingual) needs, and what this project does about it

MultiNet ships **English and Chinese only.** Thai — or anything else — will never come
from this engine. Two real options for project 3:

- **Cloud ASR** (Whisper-class). Any language, needs Wi-Fi, ~1 s latency, and a child's
  voice leaves the house. Poor fit for a toddler toy.
- **Speaker-dependent template matching** — MFCC features + dynamic time warping against
  3–5 recordings of *the child themselves* saying each word. Tiny vocabulary, one
  speaker, any language, fully offline, and it runs comfortably on an S3 without a
  neural network. This is the classic pre-deep-learning approach and it is arguably a
  *better* fit for toddlers than MultiNet, because it's trained on the actual voice.

Recommendation: project 3 is the DTW recogniser. Project 2 makes that cheap by keeping
the recogniser behind `word_event`, keeping content on SD as data, and keeping prompts as
recordings. If M3 shows MultiNet struggling with the child, the DTW route becomes
project 2's fallback rather than project 3's plan.

## Milestones — each one verified on hardware before the next

| | Milestone | Proves |
|---|---|---|
| **M0** ✅ | Mic capture at 16 kHz into PSRAM; play it back through the speaker | audio in and out work; mic gain is sane. Done 2026-09-05. |
| **M1** ✅ | MultiNet7 recognises 5 hard-coded words from an adult, logging `word=dog conf=0.87`, no wake word | the engine runs continuously on this board — risk 2. **Done 2026-09-05:** 3/3 synthesised clips, no wake word, 3.1 MB PSRAM. See the project README. |
| **M2** ✅ | `words.json` + photos on SD; word → photo on screen + chime + prompt | the whole loop, end to end. **Done 2026-09-05** on text cards with no card in the slot; the SD/photo path is written and awaits a card. |
| **M3** | The child uses it. Tune thresholds and phoneme variants on the real voice | risk 1 — the only test that matters |
| **M4** ✅ | Idle screen, brightness, power behaviour (screen dim when quiet), tap to replay | it's a toy, not a demo. **Done 2026-09-05**; dim verified on hardware, tap awaits a finger. |

M0 and M1 are the two unknowns; everything after them is assembly.

## Decisions

- **Name: `02_word_book_en`.** "Word book" is what it is — say a word, see the thing —
  and the language suffix makes the sequence read naturally: `02_word_book_en` now,
  `03_word_book_multi` (or `_th`) later, same firmware shape, different recogniser.
- **No photo → text card.** Decided above.

## Still open

1. **The word list.** Which 10–20 words — driven by which photos exist. Default until
   told otherwise: the starter list in *Recognition*.
2. **Whose voice** records the prompts, and in which language for v1 (English words,
   but the prompt voice could already be Thai — the output side doesn't care). Default:
   chime only until recordings exist; the text card covers the gap.
3. **Idle behaviour.** Blank, a slideshow of the photos, or the last photo shown?
   Default: stay on the last card.
4. **Go/no-go on M1 before M2.** If MultiNet won't run without WakeNet, pick fallback
   (parent-spoken wake word) or jump straight to the DTW recogniser.

None of these block M0 or M1.

## Sources

- ESP-SR command recognition: <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html>
- ESP-SR resource occupancy: <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html>
- ESP-SR model flashing / `model` partition: <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html>
- ESP-SR component (2.5.3, IDF ≥ 5.0): <https://components.espressif.com/components/espressif/esp-sr>
- MultiNet-without-WakeNet discussion: <https://github.com/espressif/esp-sr/issues/84>
- Arduino ESP_SR wrapper (runs command detection standalone): <https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP_SR/src/esp32-hal-sr.c>
