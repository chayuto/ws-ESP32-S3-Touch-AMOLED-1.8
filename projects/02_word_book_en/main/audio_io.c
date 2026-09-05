/*
 * I2S + ES8311 bring-up at the recogniser's format. Carried over from M0,
 * where it was verified: both directions open at 16 kHz, 48,000 samples in
 * and out in ~2.92 s each.
 */

#include "audio_io.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio_io";

/* The vendor's own example uses 90 on V2 hardware and 70 on V1. This unit is V2. */
#define SPEAKER_VOLUME 90
#define MIC_GAIN_DB    30.0f

esp_err_t audio_io_init(audio_io_t *io)
{
    /* The BSP defaults to 22,050 Hz; hand it the recogniser's format instead. */
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "bsp_audio_init");

    io->spk = bsp_audio_codec_speaker_init();
    io->mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(io->spk && io->mic, ESP_FAIL, TAG, "codec init failed: spk=%p mic=%p", io->spk, io->mic);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(io->spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "speaker open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(io->mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open");
    esp_codec_dev_set_out_vol(io->spk, SPEAKER_VOLUME);
    esp_codec_dev_set_in_gain(io->mic, MIC_GAIN_DB);

    float gain = 0;
    int vol = 0;
    esp_codec_dev_get_in_gain(io->mic, &gain);
    esp_codec_dev_get_out_vol(io->spk, &vol);
    ESP_LOGI(TAG, "ready: %d Hz mono 16-bit, mic gain %.1f dB, speaker vol %d", AUDIO_SAMPLE_RATE_HZ, gain, vol);
    return ESP_OK;
}
