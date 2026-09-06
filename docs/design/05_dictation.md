# 05_dictation — offline speech to text, printed on the glass

*Design note, 2026-09-06. Status: proposed, **feasibility unproven**.*
*Two experiments gate the whole project — see [Go/no-go](#gono-go).*

## What it is

You talk at the board. What it decoded appears on the screen as text. Nothing else.

```
   speech        16 kHz PCM         phonemes/syllables        text
  ─────────▶  [ mic → VAD ]  ──▶  [ MultiNet, unbound ]  ──▶  [ glass ]
```

**No network in the recognition path. No images. No sound.**

That last part is scope, not omission. 02 was a picture book: JPEG decode, an SD photo
cache, `words.json`, a player. **None of it comes across.** 05 draws text and only text.
Dropping it takes `esp_new_jpeg`, `book.c`, `player.c` and the photo cache out of the
build, which is most of how the debuggability below gets paid for.

**Naming.** 03 is the voice remote, 04 the multilingual word book. This is **05**.

## Offline, precisely

Battery device. No AP dependency. Works anywhere. Audio never leaves the board.

**Ruled out: streaming audio to a recogniser off-board.** It was the easy answer — reuse
02's Wi-Fi, POST a WAV to whisper.cpp, draw the reply, done in two sessions. Out on
product grounds, and the grounds are good: a battery device that only works when a Mac is
running is not a device. It is also what shipped on this board from the factory — the
Xiaozhi image runs a wake word locally and sends the rest to `api.tenclass.net`
([research note](../research/factory-firmware-20260906.md)).

**The radio still exists, for maintenance only.** Same shape as 02: long-press BOOT to
enter maintenance mode, which starts Wi-Fi and the web API; leave it and the radio stops.
The distinction that matters:

| | recognition path | maintenance mode |
| --- | --- | --- |
| radio | never on | on, deliberately, by a person |
| audio off the board | never | never |
| logs off the board | no | yes, that is the point |

Audio does not leave in either mode. Only logs do, and only when someone asks.

### A power note that shapes the design

The reason given was battery. Worth recording that **02's measured drain says the radio
was not the culprit** — 72 minutes asleep with the AFE paused but still running took the
gauge from 95 % to 46 %, the same as in use. The recogniser is the expensive thing here.

Consequence: **MultiNet must be VAD-gated, never continuous.** The AFE runs VAD only, and
the recogniser is built and torn down around detected speech. 02 already implements this
mechanism for sleep; 05 runs it per utterance. Design requirement, not optimisation.

## Why this is hard

| model | weights | vs. 8 MB PSRAM |
| --- | --- | --- |
| Whisper tiny, int8/q5 | ~30–40 MB | 4–5× over, before activations |
| Vosk small en-us | ~40 MB | 5× over |
| sherpa-onnx streaming zipformer | tens of MB | over |

Memory is the smaller obstacle: 240 MHz Xtensa LX7, 128-bit SIMD, **no NN accelerator**.
No quantisation closes a 5× memory gap and a large compute gap at once.

What the board does have is MultiNet — real, working, and closed-vocabulary with a
documented 300-command ceiling. **Option B is the attempt to get open text out of it
anyway.**

## Option B, scoped

Two independent ways to unbind MultiNet from its command list. Either would do; they are
tested separately because they fail separately.

### B1 — `raw_string` — **RUN 2026-09-06. DEAD.**

> **Result: `raw_string` is never populated.** Measured on hardware with mn7_en and a
> valid vocabulary (`vocabulary: 11 of 11 words loaded`), across thousands of decode
> frames and real detections:
>
> ```
> MX1 detected num=1 vad=0 vol=-52.5 frame=1097 string=' STnP' raw_string=''
> MX1 detected num=1 vad=1 vol=-55.3 frame=1193 string=' cP'   raw_string=''
> MX1 summary: detect states [detecting 2495 detected .. timeout 14],
>              probe calls 14, raw_string set 0, raw differs 0
> ```
>
> `string` does carry a phoneme sequence — `STnP` for "stop", `cP` for "up" — but it is
> the *matched command's* phonemes, which is exactly what "with commands graph" means.
> The field documented as the decode without the graph stays empty on every result,
> detected and timed out alike. The struct exposes it; MultiNet7 does not fill it.
>
> The probe deliberately read results on `ESP_MN_STATE_TIMEOUT` as well as `DETECTED`,
> since out-of-vocabulary speech lands there — so this is not a case of looking in the
> wrong branch. **B1 is closed. The project now rests entirely on B2.**

The original reasoning, kept because it is why the experiment was worth running:


`esp_mn_results_t` returns more than command IDs:

```c
char string[256];        // recognized string with commands graph
char raw_string[256];    // recognized string WITHOUT commands graph
```

MultiNet decodes in phonemes — 02's boot log prints its vocabulary that way
(`Command 7: DeG` for DOG, `Command 6: KaT` for CAT). `string` is snapped to the command
list; **`raw_string` is documented as the decode without it.** If it carries a
free-running phoneme sequence, then phonemes plus a pronunciation dictionary on the SD
card is a transcript, and the 300-command cap stops applying.

The header names three decoders; 02 runs the most constrained (`search method: 2`):

```c
ESP_MN_GREEDY_SEARCH = 0,
ESP_MN_BEAM_SEARCH = 1,
ESP_MN_BEAM_SEARCH_WITH_FST = 2,
```

**Why it may fail:** no public API sets the search method; MN7 is trained on short
commands with a 5.0 s timeout, not continuous speech; phoneme→word without a language
model is error-prone even given clean phonemes; `raw_string` may be empty in FST mode.

### B2 — load syllables, not words — **ENGLISH CONTROL RUN 2026-09-07. POSITIVE.**

> **MultiNet streams.** A 4.73 s synthesised sentence, injected through
> `recognizer_inject()` so the test is repeatable, against the 187-word English
> control table. MultiNet's timeout is 5 s, so the whole sentence sat inside ONE
> utterance window:
>
> ```
> MX2 +0 ms     'the'  p=0.182  (run 1, #1 in run)
> MX2 +579 ms   'and'  p=0.148  (run 1, #2 in run)
> MX2 +860 ms   'to'   p=0.170  (run 1, #3 in run)
> MX2 +1124 ms  'it'   p=0.146  (run 1, #4 in run)
> MX2 +575 ms   'the'  p=0.151  (run 1, #5 in run)
> MX2 +1061 ms  'the'  p=0.147  (run 1, #6 in run)
> MX2 summary: 1 speech runs, best run 6 detections, gaps 575-1124 ms
> ```
>
> **Six detections inside one utterance.** The engine does not stop at one command
> and wait for a timeout - it keeps firing. That was the structural question and it
> is answered: **B2 is the path.**
>
> **The accuracy is bad, and that is the real finding.** The sentence was "the first
> time we went down to the water and they said it would take three more time to get
> back home" - roughly 20 words. It returned 6, all function words, with "the" three
> times, and every confidence between 0.146 and 0.182. All of them fell below the
> 0.20 floor, so nothing reached the screen. Roughly one word in four, and the ones
> it caught are the ones that carry no meaning.
>
> So the mechanism works and the transcript does not. That is a tuning and
> language-model problem rather than an architectural one, which is a much better
> place to be - but it is not a working dictation device, and the gap should not be
> understated.
>
> Caveat: the clip is TTS, not a human. Espressif's own wake words are trained on
> TTS (`wn9_*_tts`), so it is a fair proxy, but a human voice should be measured
> before any of these numbers are trusted as a baseline.

**Mandarin run: blocked, not yet answered.** See [Still open](#still-open).

The original reasoning:


MultiNet takes commands as **phoneme strings** (`esp_mn_commands_phoneme_add`), several
phrases may share a `command_id`, and `check_speech_command` reports up front whether a
string tokenises. So load 300 *syllables* instead of 300 *words*, and let continuous
speech fire them one after another. The output is a syllable stream, assembled into text
with a dictionary on the card.

**This is where language stops being free.** Mandarin has ~400 base syllables ignoring
tone — close to the 300 ceiling. English syllable inventory runs to the thousands and
does not fit.

> **For offline dictation on this chip, Chinese is structurally easier than English.**
> If B2 wins, the project is Chinese.

**Why it may fail:** MultiNet detects *one* command per utterance then times out, rather
than emitting a stream; per-syllable confidence will be poor with no language model above
it; and 300 < 400.

### If both fail

05 becomes a 300-command phrase board — offline, working, and not dictation — or is
shelved pending research into an ESP32-P4 class part. Stated here so the fallback is a
decision and not a disappointment.

## Go/no-go

**Feasibility is unknown and cheap to establish.** Both experiments run inside
`02_word_book_en`, on hardware that already works. Nothing new is built until they answer.

- **MX-1 — DONE 2026-09-06, negative.** `raw_string` is always empty; see B1 above.
  Originally scoped as:
- **MX-1 — `raw_string`, ~10 minutes.** One `ESP_LOGI` of `res->raw_string` beside the
  existing result handling in `main/recognizer.c`. Flash. Say things outside the 20-word
  list. Read the serial.
- **MX-2 — syllables as commands, ~1–2 hours.** Load ~200 Mandarin syllables via
  `esp_mn_commands_phoneme_add`, gate on `check_speech_command`, speak continuously, log
  every detection with timing.

**Do not start M0 before both have answered.**

## Architecture

```
ES8311 mic ──I2S 16 kHz mono──▶ AFE (VAD only, always on, cheap)
                                      │  speech detected
                                      ▼
                        build MultiNet ──▶ decode ──▶ phoneme/syllable stream
                                      │                         │
                        tear down on silence                    ▼
                                                   dictionary on SD ──▶ text
                                                                        │
                                             LVGL transcript view ◀─────┘
```

## Debuggability — ported wholesale from 02

02's real achievement is not the word book, it is that when it misbehaves you can find
out why without guessing. All of it comes across. It is listed explicitly because "port
the debuggability" is easy to under-deliver on.

| from 02 | what it buys here |
| --- | --- |
| **`clog.c` + levelled logging** | every subsystem tagged, verbosity switchable at runtime |
| **`sdlog.c` flight recorder** | append-only log on the card, survives a reboot |
| **`.jsonl` record streams** | 02 wrote classifier + state records; 05 writes one **decode record per utterance** |
| **`maint.c` web API** | `/api/metrics`, `/api/log`, `/api/state`, `/api/reload`, `/api/reboot` — read the board from a browser |
| **`devcmd.c` serial commands** | `m` maintenance, `i` info, `d` debug, `p` power, `b` bright, `x` panel re-init, `r` reload |
| **`pmu.c` power telemetry** | rails, battery, charge state at boot, in the heartbeat, on every VBUS change |
| **`thermal.c`** | trip lines and cool-down, with the AMOLED lit and inference running |
| **boot diagnostics** | reset cause, RTC validity, crash streak, **safe mode after 3 crashes** |
| **`cards.c`** | display control **including the checked-brightness and panel re-init fix** |
| **heartbeat line** | one periodic line stating what the board thinks its own state is |

### The decode record

The single most valuable new artefact, and the direct descendant of 02's classifier
records — which is how the black-panel afternoon was actually diagnosed. One JSON line
per utterance, on the card:

```
timestamp · vad open/close ms · audio level avg+peak · raw_string · matched syllables
· per-token confidence · dictionary hit or miss · final text · decode ms · free heap
```

Because accuracy will be the whole argument in this project, **every decode must be
reconstructable after the fact.** Storing the audio for failed decodes is worth
considering too — the card has 13 GB free.

### What maintenance mode must show

Beyond 02's set: last N decodes with confidences, live audio level, VAD state, dictionary
size and hit rate, and MultiNet build/tear-down timing per utterance.

## Space — "if space allows"

02 finished at `0x3bc6b0` in a 4 MB app partition, **7 % free**. That was uncomfortable,
and it is why the question was asked. The answer is to not repeat it.

**Size the partitions properly at project start.** This is a new project and a full flash
either way:

```
nvs      data nvs           24K
phy_init data phy            4K
factory  app  factory        8M      <- 2x what 02 fought over
model    data spiffs         6M      <- mn7_en or mn7_cn, unchanged
storage  data spiffs         1M
                          ------
                          ~15.1M of 16M
```

Fits, with ~0.9 MB spare. And 05 starts lighter than 02 anyway — no JPEG decoder, no
photo cache, no player, no `book.c`.

**So the honest answer to "if space allows" is: it allows.** Port all of it. The
dictionary and any font live on the SD card, not in flash.

## Budget

| | 02 today | 05 |
| --- | --- | --- |
| app partition | 4 MB, 7 % free | **8 MB** |
| model partition | 6 MB (2.9 MB used) | same |
| PSRAM after load | 4.13 MB free | similar, plus dictionary buffers |
| internal RAM free | **72 KB** | the ceiling; watch it |
| dictionary, font | — | SD card |

Internal RAM stays the real constraint. Wi-Fi and the web server only cost it while
maintenance mode is running, which is the argument for keeping them out of the normal
path regardless of the offline requirement.

## The display

Text only. `lv_label` with `LV_LABEL_LONG_WRAP` in a scrollable container, plus explicit
**listening / working / done** states — 02's hardest-won lesson is that a screen which
shows nothing is indistinguishable from a dead one.

**English is free** (Montserrat ships with LVGL, UTF-8 native). **Chinese costs a font**,
and B2 points at Chinese: LVGL's bundled CJK face covers only **1,118 ideographs**;
running text wants ~3,000–3,500. Build one with `lv_font_conv` from Noto Sans SC
(~0.6 MB at 16 px) and load it from the card at runtime via LVGL 9's `binfont_loader` —
zero flash cost. A missing glyph renders as a box; a malformed string never reaches LVGL.

## Reuse from 02

**Comes across:** `sdcard.c` · `sdlog.c` · `clog.c` · `audio_io.c` · `recognizer.c`
(heavily) · `maint.c` · `devcmd.c` · `wifi_sta.c` (maintenance only) · `pmu.c` ·
`thermal.c` · `timesync.c` · `pcf85063.c` · `cards.c` · `button.c`

**Does not:** `book.c` · `player.c` · JPEG decode · the photo cache · `words.json`

Roughly two thirds of the plumbing exists and is debugged on this hardware, including
every trap that cost a day.

## Difficulty verdict

**The plumbing is two or three sessions. The recogniser is a research question with a
plausible answer of "no".**

That asymmetry is the finding. Offline is the right product call and it moved this
project from *boring* to *unproven*. Spend MX-1 and MX-2 before budgeting anything else.

## Risks, ranked

1. **It may not be possible.** A risk to the project, not the schedule. MX-1 and MX-2
   exist to learn this in an afternoon rather than a month.
2. **Battery.** The measured culprit was the recogniser, not the radio. If MultiNet must
   run near-continuously to catch syllables, offline buys nothing. **Instrument power
   from M0**, not at the end.
3. **Accuracy will be poor** even in the good case. A 60 %-correct transcript reads as
   broken. Decide early what the screen does at low confidence — showing uncertainty
   honestly beats printing confident nonsense.
4. **Chinese pulls in a font and a dictionary**, both new build steps.
5. **Thermal** — sustained inference with the AMOLED lit. `thermal.c` transfers; its trip
   lines still want calibrating against a thermometer on the glass.

## Milestones — only after Go/no-go

- **M0** — skeleton, no radio in the normal path: BSP, VAD-gated MultiNet build/tear-down,
  **logging, flight recorder and power telemetry in the first commit**, not retrofitted.
- **M1** — the winning decode path from MX, logged over serial. No screen yet.
- **M2** — dictionary on SD, stream → text, still serial. Decode records written.
- **M3** — transcript on the AMOLED with listening / working / done states.
- **M4** — maintenance mode and the web API.
- **M5** — font work if the answer was Chinese.

## Decisions

1. **Text only.** No images, no sound. Scope, not a later addition.
2. **Offline recognition path.** Radio exists solely for maintenance mode, started by a
   person. Audio never leaves the board in any mode.
3. **MultiNet is VAD-gated**, never continuous. Power requirement.
4. **8 MB app partition from day one.** Do not repeat 02's 7 %.
5. **Debuggability ships in M0**, not after it works.
6. **Language follows the method.** If B2 wins, the project is Chinese.
7. **No new hardware** until B1 and B2 are both dead.

## Still open

- Whether to keep audio for failed decodes on the card. Probably yes; costs nothing.
- Whether a small n-gram language model over syllables fits in remaining PSRAM. It is the
  difference between a syllable stream and readable text, and it has not been sized.
- Whether the transcript persists across reboots.
- What an ESP32-P4 with more PSRAM could actually run. Unresearched.

## Sources

- `esp-sr` 2.5.3, `include/esp32s3/esp_mn_iface.h` — `esp_mn_results_t`, search methods,
  `esp_mn_commands_phoneme_add`, `check_speech_command`
- [`../research/speech-stack-20260906.md`](../research/speech-stack-20260906.md) — model
  inventory, measured memory, the `raw_string` thread, CJK font counts
- [`../research/factory-firmware-20260906.md`](../research/factory-firmware-20260906.md)
- `projects/02_word_book_en/` — reusable modules, boot log, the 95 %→46 % power
  measurement, and the `0x3bc6b0` / 7 %-free app size
