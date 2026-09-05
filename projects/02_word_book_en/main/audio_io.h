#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

/* The recogniser's format. Everything in the audio path uses it. */
#define AUDIO_SAMPLE_RATE_HZ 16000

typedef struct {
    esp_codec_dev_handle_t mic;
    esp_codec_dev_handle_t spk;
} audio_io_t;

/* Bring up I2S at 16 kHz mono and open both ES8311 directions. */
esp_err_t audio_io_init(audio_io_t *io);
