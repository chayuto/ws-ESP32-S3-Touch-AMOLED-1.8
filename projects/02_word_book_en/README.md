# 02_word_book_en

A voice-triggered picture book for a toddler: say a word, see the photo, hear it back.
Design: [`docs/design/02_word_book_en.md`](../../docs/design/02_word_book_en.md).

Built in milestones, each verified on hardware before the next.

| | Milestone | Status |
|---|---|---|
| **M0** | Mic → PSRAM → speaker at 16 kHz mono | **done 2026-09-05** |
| **M1** | MultiNet7 recognises words, no wake word | **done 2026-09-05** — see below |
| M2 | `words.json` + photos on SD; word → card + chime + prompt | |
| M3 | The child uses it; tune on the real voice | |
| M4 | Idle screen, dimming, make it a toy | |

## Build, flash, watch

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/02_word_book_en -B /tmp/ws-amoled-build/02_word_book_en build
idf.py -C projects/02_word_book_en -B /tmp/ws-amoled-build/02_word_book_en -p /dev/cu.usbmodem3101 flash
~/.espressif/python_env/idf5.5_py3.14_env/bin/python .claude/skills/serial-capture/scripts/capture.py --seconds 16
```

## M1 — continuous recognition, no wake word

The ESP-SR pipeline (`main/recognizer.c`): mic → AFE (VAD) → MultiNet7 English →
`word_event_t` on a queue. Two tasks on core 1, feed and detect. The AFE is created with
`wakenet_init = false` and MultiNet runs on every frame.

**Finding: the docs' "MultiNet must be used with WakeNet" is a recommendation, not a
rule.** It runs continuously without one. The AFE warns once at start-up
(`wakenet model not found`) and carries on.

### Self-test, so it can be proven with nobody in the room

Three clips synthesised on the Mac (`say -v Samantha`, resampled to 16 kHz mono, padded
with silence) are embedded in the app and fed *through the recogniser* at boot, exactly as
if the mic had heard them. Verified output, 2026-09-05:

```
I (1106) recog: model[0]: mn7_en
I (1112) AFE: AFE Pipeline: [input] -> |VAD(WebRTC)| -> [output]
I (1687) recog: engine loaded: internal -22376 B (179563 free), psram -3101900 B (5134196 free)
I (1688) recog: listening for 8 words, no wake word
I (3200) recog: inject 'dog': 34466 samples (2.15 s)
I (4751) recog: detected [1/1] id=0 'DOG' prob=0.725  (vad=1 vol=-69.0 dBFS, frame 98)
I (5615) recog: detected [1/1] id=7 'CAR' prob=0.135  (vad=0 vol=-54.6 dBFS, frame 127)
I (5616) recog: rejected: detected on silence (prob 0.135, floor 0.20)
I (6850) wordbook: self-test 1/3: clip 'dog' -> 'DOG'  [PASS]
I (8400) recog: detected [1/1] id=1 'CAT' prob=0.826  (vad=1 vol=-68.0 dBFS, frame 214)
I (10608) wordbook: self-test 2/3: clip 'cat' -> 'CAT'  [PASS]
I (12065) recog: detected [1/1] id=2 'BALL' prob=0.691  (vad=1 vol=-67.5 dBFS, frame 331)
I (14263) wordbook: self-test 3/3: clip 'ball' -> 'BALL'  [PASS]
I (14271) wordbook: self-test: 3/3 clips recognised
```

What it establishes:

- **Engine cost:** 3.10 MB PSRAM, 22 KB internal — the design note predicted 2.9 + 0.3 MB.
  179 KB internal and 5.13 MB PSRAM remain.
- **Latency:** each clip has 0.6 s of lead silence and a ~0.35 s word; detection fires
  ~1.55 s in, so about **600 ms after the word ends**.
- **Vocabulary is graphemes.** `esp_mn_commands_add(id, "DOG")` — the component's built-in
  `flite_g2p` derives the phonemes. `esp_mn_commands_phoneme_add()` is there for hand-spelled
  toddler variants later.
- **Silence produces junk, and VAD catches it.** After every clip the zero-padded tail made
  MultiNet emit `CAR` at exactly 0.135 with `vad=0`. Real words fired with `vad=1` (the VAD
  holds "speech" for a second after it ends). So the gate is *VAD says speech* plus a low
  floor of 0.20 — not a high probability threshold, which would have discarded a genuine
  `BALL` that came in at 0.284 on one run. Run-to-run probabilities vary (BALL: 0.68, 0.28,
  0.69); the engine is not deterministic across boots.

Not yet verified: live speech from a person. The path is identical to the injected one
from the AFE onward; only the mic capture level is untested, and M0 showed that live.

## M0 — audio loopback

Two seconds after boot the board records three seconds from the onboard mic into PSRAM,
logs the level, and plays it back through the speaker. Tap the screen to run it again.
The screen shows the state (RECORDING / PLAYING / IDLE) and the measured level.

Format is deliberately the recogniser's: **16 kHz, 16-bit, mono.** The BSP defaults to
22,050 Hz; `audio_setup()` hands `bsp_audio_init()` its own `i2s_std_config_t` instead.

### Verified output (2026-09-05, nobody in the room)

```
I (723) ES8311: Work in Slave mode
I (735) I2S_IF: STD: TX, sample_rate_hz: 16000, mclk_multiple: 256, clk_src: 6
I (736) I2S_IF: STD: RX, sample_rate_hz: 16000, mclk_multiple: 256, clk_src: 6
I (752) Adev_Codec: Open codec device OK
I (769) Adev_Codec: Open codec device OK
I (770) wordbook: audio ready: 16000 Hz mono 16-bit, mic gain 30.0 dB, speaker vol 90
I (770) wordbook: record buffer: 96000 bytes in PSRAM, audio task on core 1
I (5729) wordbook: recorded 48000 samples in 2917 ms (expected 3000): peak=232 (-43.0 dBFS) rms=53 (-55.9 dBFS)
I (8656) wordbook: played 48000 samples in 2919 ms
I (10716) wordbook: alive: taps=0 internal=248723 psram=8139836
```

What that proves:

- Both codec directions open at 16 kHz with no error or warning.
- Capture and playback each consume 48,000 samples in ~2.92 s — the I²S clock is right
  (the ~80 ms shortfall is the first DMA buffer already being full when the read starts).
- The mic path is live: −43 dBFS peak in an empty room is ambient noise, not a dead
  input (a dead input reads 0 or a fixed DC value; the code flags anything under 50).
- PSRAM cost is exactly the 96,000-byte buffer (8,237,204 → 8,139,836).

What it does **not** prove yet — needs a person at the board:

- That the mic captures **speech** at a usable level. Expect roughly −20 to −10 dBFS
  peak talking at arm's length; if it's much lower, raise `MIC_GAIN_DB`.
- That the playback is **audible** and clean. The DAC accepted the data at the right
  rate; whether the amp and speaker produced sound was not witnessed.

Tap the screen with someone talking, and both are answered in six seconds.

### Settings

| | Value | Why |
|---|---|---|
| `SPEAKER_VOLUME` | 90 | The vendor example uses 90 on V2 hardware, 70 on V1 |
| `MIC_GAIN_DB` | 30 | Starting point; tune from the speech-level test above |
| Audio task | core 1, priority 5 | Away from LVGL on core 0; the recogniser will share this task |

## Layout

```
main/app_main.c        UI, self-test, event loop
main/audio_io.[ch]     I2S + ES8311 at 16 kHz mono (from M0)
main/recognizer.[ch]   ESP-SR AFE + MultiNet behind word_event_t — the module project 3 replaces
main/testclips/*.pcm   synthesised 16 kHz clips embedded for the boot self-test
partitions.csv         factory 4M / model 6M / storage 1M
sdkconfig.defaults     board baseline + CONFIG_SR_MN_EN_MULTINET7_QUANT + CONFIG_MODEL_IN_FLASH
```

`idf.py flash` writes `srmodels.bin` (2.76 MB: mn7_en + its FST) to the `model` partition
alongside the app. Changing `sdkconfig.defaults` needs the generated `sdkconfig` deleted
first, as always.
