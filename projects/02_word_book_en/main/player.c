/*
 * Speaker output. Recognition is paused while anything plays so the board
 * does not listen to itself; no echo cancellation needed for a chime and a
 * one-word prompt.
 */

#include "player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "recognizer.h"

static const char *TAG = "player";

static esp_codec_dev_handle_t s_spk;

#define CHUNK_SAMPLES 1024

void player_init(esp_codec_dev_handle_t spk)
{
    s_spk = spk;
}

static void write_samples(const int16_t *samples, size_t count)
{
    size_t sent = 0;
    while (sent < count) {
        size_t n = count - sent;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        int ret = esp_codec_dev_write(s_spk, (void *)(samples + sent), (int)(n * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "write failed: %d", ret);
            return;
        }
        sent += n;
    }
}

static size_t tone(int16_t *out, float hz, int ms, float amp)
{
    size_t n = (size_t)AUDIO_SAMPLE_RATE_HZ * ms / 1000;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / AUDIO_SAMPLE_RATE_HZ;
        float env = fminf(1.0f, (float)i / (AUDIO_SAMPLE_RATE_HZ * 0.005f)) * expf(-4.0f * (float)i / n);
        out[i] = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * hz * t));
    }
    return n;
}

void player_chime(void)
{
    static int16_t *buf;
    static size_t len;
    if (buf == NULL) {
        buf = heap_caps_malloc(AUDIO_SAMPLE_RATE_HZ * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* 1 s max */
        if (buf == NULL) {
            return;
        }
        /* G5 then C6: two short notes, a rising "yes". */
        len = tone(buf, 784.0f, 110, 11000.0f);
        len += tone(buf + len, 1046.5f, 200, 11000.0f);
    }
    recognizer_pause();
    write_samples(buf, len);
    recognizer_resume();
}

esp_err_t player_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Minimal RIFF walk: find "fmt " and "data". */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "%s: not a WAV", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0, data_len = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t size = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) {
                break;
            }
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            fseek(f, (long)size - 16, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_len = size;
            break;
        } else {
            fseek(f, (long)size + (size & 1), SEEK_CUR);
        }
    }
    if (data_len == 0 || channels != 1 || rate != AUDIO_SAMPLE_RATE_HZ || bits != 16) {
        ESP_LOGW(TAG, "%s: need 16 kHz mono 16-bit, got %u Hz %u ch %u bit", path, (unsigned)rate, channels, bits);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    int16_t *buf = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    recognizer_pause();
    uint32_t left = data_len;
    while (left > 0) {
        size_t want = left < CHUNK_SAMPLES * sizeof(int16_t) ? left : CHUNK_SAMPLES * sizeof(int16_t);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        write_samples(buf, got / sizeof(int16_t));
        left -= got;
    }
    recognizer_resume();
    free(buf);
    fclose(f);
    ESP_LOGI(TAG, "played %s (%u samples)", path, (unsigned)(data_len / 2));
    return ESP_OK;
}
