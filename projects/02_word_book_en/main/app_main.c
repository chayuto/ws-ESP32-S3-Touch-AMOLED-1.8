/*
 * 02_word_book_en — M0: audio loopback.
 *
 * Proves the microphone and speaker paths at the format the recogniser will
 * need: 16 kHz, 16-bit, mono. Records three seconds into PSRAM, reports the
 * level, plays it straight back. One cycle runs automatically after boot so the
 * result is visible on serial alone; tapping the screen runs another.
 *
 * No SD card, no recogniser, no photos yet. Those arrive in M1 and M2.
 */

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "wordbook";

/* Recogniser format: MultiNet wants 16 kHz / 16-bit / mono, so M0 tests exactly that. */
#define SAMPLE_RATE_HZ   16000
#define RECORD_SECONDS   3
#define RECORD_SAMPLES   (SAMPLE_RATE_HZ * RECORD_SECONDS)
#define RECORD_BYTES     (RECORD_SAMPLES * sizeof(int16_t))
#define CHUNK_SAMPLES    512

/* The vendor's own example uses 90 on V2 hardware and 70 on V1. This unit is V2. */
#define SPEAKER_VOLUME   90
#define MIC_GAIN_DB      30.0f

/* --- UI ------------------------------------------------------------------ */

static lv_obj_t *s_state_label;
static lv_obj_t *s_stats_label;
static SemaphoreHandle_t s_run_cycle;
static uint32_t s_tap_count;

static void ui_set_state(const char *text, lv_color_t color)
{
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_state_label, text);
        lv_obj_set_style_text_color(s_state_label, color, 0);
        bsp_display_unlock();
    }
}

static void ui_set_stats(const char *text)
{
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_stats_label, text);
        bsp_display_unlock();
    }
}

static void screen_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_tap_count++;
    ESP_LOGI(TAG, "tap #%" PRIu32 " -> requesting a record/playback cycle", s_tap_count);
    xSemaphoreGive(s_run_cycle);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_pressed_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "word book  -  M0 audio");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8899aa), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    s_state_label = lv_label_create(scr);
    lv_label_set_text(s_state_label, "starting");
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_white(), 0);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, -40);

    s_stats_label = lv_label_create(scr);
    lv_label_set_text(s_stats_label, "");
    lv_obj_set_style_text_font(s_stats_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_stats_label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_align(s_stats_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_stats_label, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "tap to record 3 s and play it back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x667788), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);
}

/* --- Audio --------------------------------------------------------------- */

typedef struct {
    int16_t peak;
    float rms;
} level_t;

static level_t measure_level(const int16_t *samples, size_t count)
{
    level_t lv = {0};
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        int16_t s = samples[i];
        int16_t mag = (int16_t)(s < 0 ? -s : s);
        if (mag > lv.peak) {
            lv.peak = mag;
        }
        sum_sq += (double)s * (double)s;
    }
    lv.rms = count ? (float)sqrt(sum_sq / (double)count) : 0.0f;
    return lv;
}

static float to_dbfs(float linear)
{
    return linear > 0.0f ? 20.0f * log10f(linear / 32768.0f) : -120.0f;
}

static esp_err_t audio_setup(esp_codec_dev_handle_t *mic, esp_codec_dev_handle_t *spk)
{
    /* The BSP defaults to 22,050 Hz; hand it the recogniser's format instead. */
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
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

    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    if (*spk == NULL || *mic == NULL) {
        ESP_LOGE(TAG, "codec init failed: spk=%p mic=%p", *spk, *mic);
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE_HZ,
        .mclk_multiple = 256,
    };
    int ret = esp_codec_dev_open(*spk, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(*mic, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed: %d", ret);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(*spk, SPEAKER_VOLUME);
    esp_codec_dev_set_in_gain(*mic, MIC_GAIN_DB);

    float gain = 0;
    int vol = 0;
    esp_codec_dev_get_in_gain(*mic, &gain);
    esp_codec_dev_get_out_vol(*spk, &vol);
    ESP_LOGI(TAG, "audio ready: %d Hz mono 16-bit, mic gain %.1f dB, speaker vol %d", SAMPLE_RATE_HZ, gain, vol);
    return ESP_OK;
}

static void run_cycle(esp_codec_dev_handle_t mic, esp_codec_dev_handle_t spk, int16_t *buf)
{
    char stats[128];

    /* Record */
    ui_set_state("RECORDING", lv_color_hex(0xff4444));
    ui_set_stats("say something");
    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    while (got < RECORD_SAMPLES) {
        size_t want = RECORD_SAMPLES - got;
        if (want > CHUNK_SAMPLES) {
            want = CHUNK_SAMPLES;
        }
        int ret = esp_codec_dev_read(mic, buf + got, want * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic read failed at sample %u: %d", (unsigned)got, ret);
            ui_set_state("MIC ERROR", lv_color_hex(0xff8800));
            return;
        }
        got += want;
    }
    int64_t rec_us = esp_timer_get_time() - t0;

    level_t lv = measure_level(buf, RECORD_SAMPLES);
    ESP_LOGI(TAG, "recorded %u samples in %" PRId64 " ms (expected %d): peak=%d (%.1f dBFS) rms=%.0f (%.1f dBFS)",
             (unsigned)got, rec_us / 1000, RECORD_SECONDS * 1000, lv.peak, to_dbfs(lv.peak), lv.rms, to_dbfs(lv.rms));

    /* A dead mic reads all zeros or a fixed DC offset; live air is never that quiet. */
    const char *verdict = (lv.peak < 50) ? "silent - mic dead?" : (lv.peak < 800) ? "quiet room" : "signal";
    snprintf(stats, sizeof(stats), "peak %.1f dBFS\nrms %.1f dBFS\n%s", to_dbfs(lv.peak), to_dbfs(lv.rms), verdict);
    ui_set_stats(stats);

    /* Play back */
    ui_set_state("PLAYING", lv_color_hex(0x44ff88));
    t0 = esp_timer_get_time();
    size_t sent = 0;
    while (sent < RECORD_SAMPLES) {
        size_t n = RECORD_SAMPLES - sent;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        int ret = esp_codec_dev_write(spk, buf + sent, n * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "speaker write failed at sample %u: %d", (unsigned)sent, ret);
            ui_set_state("SPK ERROR", lv_color_hex(0xff8800));
            return;
        }
        sent += n;
    }
    int64_t play_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "played %u samples in %" PRId64 " ms", (unsigned)sent, play_us / 1000);

    ui_set_state("IDLE", lv_color_white());
}

static void audio_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t mic = NULL, spk = NULL;

    if (audio_setup(&mic, &spk) != ESP_OK) {
        ui_set_state("AUDIO INIT FAILED", lv_color_hex(0xff8800));
        vTaskDelete(NULL);
        return;
    }

    int16_t *buf = heap_caps_malloc(RECORD_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "could not allocate %u bytes in PSRAM", (unsigned)RECORD_BYTES);
        ui_set_state("NO PSRAM", lv_color_hex(0xff8800));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "record buffer: %u bytes in PSRAM, audio task on core %d", (unsigned)RECORD_BYTES, xPortGetCoreID());

    /* One unattended cycle so a serial-only check still sees a result. */
    ui_set_state("IDLE", lv_color_white());
    vTaskDelay(pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_run_cycle);

    for (;;) {
        if (xSemaphoreTake(s_run_cycle, portMAX_DELAY) == pdTRUE) {
            /* Drop taps that queued up during the cycle; one run per press is enough. */
            run_cycle(mic, spk, buf);
            while (xSemaphoreTake(s_run_cycle, 0) == pdTRUE) {}
        }
    }
}

/* --- Entry ---------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "02_word_book_en M0 starting");

    s_run_cycle = xSemaphoreCreateBinary();

    /* The BSP's i2c.master pull-up warning is expected on this board; keep the log clean. */
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    lv_display_t *disp = bsp_display_start();
    esp_log_level_set("i2c.master", ESP_LOG_INFO);
    bsp_display_brightness_set(85);
    ESP_LOGI(TAG, "display up: %" PRId32 "x%" PRId32, lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));

    if (bsp_display_lock(1000)) {
        build_ui();
        bsp_display_unlock();
    }

    /* Audio on core 1, away from LVGL; this is where the recogniser will live too. */
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 5, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive: taps=%" PRIu32 " internal=%u psram=%u", s_tap_count,
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
