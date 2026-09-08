#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Ambient sound LEVEL. A number, not a recording.
 *
 * ---------------------------------------------------------------------------
 * What this is not, and how the code guarantees it rather than promising it:
 *
 *   - The microphone is closed between measurements. esp_codec_dev_close() at
 *     the end of every burst, opened again at the start of the next. Between
 *     them the ES8311's ADC is not running and the I2S channel is not clocked,
 *     so there is no capture happening at all - not into a DMA ring, not
 *     anywhere. The board listens for CONFIG_SENSOROUS_SOUND_MS out of every
 *     cadence period and is deaf for the rest.
 *
 *   - Audio never leaves this file. Samples are read in 512-sample chunks into
 *     one small static buffer, folded into four running sums, and overwritten
 *     by the next chunk. Nothing is concatenated, nothing is kept, and the
 *     largest amount of audio in existence at any moment is 1 KB that no other
 *     module can reach.
 *
 *   - Nothing derived from the audio but the four numbers below is ever
 *     written to the card or served over HTTP. There is no VAD, no recogniser,
 *     no spectrum - a level is not speech, and the path to make it speech does
 *     not exist in this build.
 * ---------------------------------------------------------------------------
 *
 * The scale is dBFS, not dB SPL: 0 dBFS is a full-scale square wave, so a
 * full-scale sine reads -3.0. It is an uncalibrated electrical level, and the
 * gain in front of it is CONFIG_SENSOROUS_MIC_GAIN_DB. Two readings from this
 * board compare with each other; neither compares with a sound level meter
 * without someone doing a calibration first.
 */

typedef struct {
    bool valid;
    float rms_dbfs;   /* DC removed first - this is a level, not an offset */
    float peak_dbfs;  /* loudest single sample in the window */
    uint32_t samples;
    uint32_t clipped; /* samples at or near full scale: the reading is a floor, not a value */
    uint32_t errors;
    uint32_t ms;      /* how long the whole burst took, open and close included */
} sound_level_t;

/* Bring up I2S and the ES8311 microphone, then close it again. */
esp_err_t sound_init(void);

/* False when the codec did not answer at boot; sound_measure() then reports nothing. */
bool sound_present(void);

/*
 * Open the mic, discard the settling window, measure, close. Blocks for roughly
 * CONFIG_SENSOROUS_SOUND_SETTLE_MS + CONFIG_SENSOROUS_SOUND_MS.
 * Returns false if the burst failed; `out` is still filled in with valid=false.
 */
bool sound_measure(sound_level_t *out);

/* The last measurement, without taking a new one. For the screen. */
void sound_last(sound_level_t *out);

/* Bursts taken and bursts that failed, for the run. */
void sound_counts(uint32_t *bursts, uint32_t *failures);
