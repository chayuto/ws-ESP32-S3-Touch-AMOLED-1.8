#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

void player_init(esp_codec_dev_handle_t spk);

/* Short two-note chime, synthesised. Blocks for ~0.3 s. */
void player_chime(void);

/* Play a 16 kHz mono 16-bit WAV file. Blocks until done. */
esp_err_t player_wav(const char *path);
