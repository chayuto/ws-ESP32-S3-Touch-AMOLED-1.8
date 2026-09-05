/*
 * 02_word_book_en — M1: continuous recognition, no wake word.
 *
 * Boots the display, brings the codec up at 16 kHz, starts the ESP-SR pipeline
 * on core 1, then runs a self-test: three synthesised clips ("dog", "cat",
 * "ball") are fed through the recogniser as if the mic had heard them. That
 * proves the engine without anyone in the room. After that it listens live and
 * shows every word it hears.
 */

#include <inttypes.h>
#include <string.h>

#include "audio_io.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "recognizer.h"

static const char *TAG = "wordbook";

/* Starter vocabulary. Grows from words.json on the SD card in M2. */
static const word_def_t s_words[] = {
    {"DOG"}, {"CAT"}, {"BALL"}, {"MAMA"}, {"DADA"}, {"DUCK"}, {"BABY"}, {"CAR"},
};
#define WORD_COUNT (sizeof(s_words) / sizeof(s_words[0]))

/* Self-test clips: macOS `say` at 16 kHz mono, padded with silence, raw PCM. */
extern const uint8_t clip_dog_start[] asm("_binary_dog_pcm_start");
extern const uint8_t clip_dog_end[] asm("_binary_dog_pcm_end");
extern const uint8_t clip_cat_start[] asm("_binary_cat_pcm_start");
extern const uint8_t clip_cat_end[] asm("_binary_cat_pcm_end");
extern const uint8_t clip_ball_start[] asm("_binary_ball_pcm_start");
extern const uint8_t clip_ball_end[] asm("_binary_ball_pcm_end");

typedef struct {
    const char *label;
    const char *expect;
    const uint8_t *start, *end;
} clip_t;

static const clip_t s_clips[] = {
    {"dog", "DOG", clip_dog_start, clip_dog_end},
    {"cat", "CAT", clip_cat_start, clip_cat_end},
    {"ball", "BALL", clip_ball_start, clip_ball_end},
};
#define CLIP_COUNT (sizeof(s_clips) / sizeof(s_clips[0]))

static lv_obj_t *s_word_label;
static lv_obj_t *s_sub_label;
static lv_obj_t *s_log_label;
static QueueHandle_t s_events;
static uint32_t s_heard;

static void ui_show(const char *word, const char *sub)
{
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_word_label, word);
        lv_label_set_text(s_sub_label, sub);
        bsp_display_unlock();
    }
}

static void ui_log(const char *line)
{
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_log_label, line);
        bsp_display_unlock();
    }
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "word book  -  M1 listen");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8899aa), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    s_word_label = lv_label_create(scr);
    lv_label_set_text(s_word_label, "...");
    lv_obj_set_style_text_font(s_word_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_word_label, lv_color_white(), 0);
    lv_obj_align(s_word_label, LV_ALIGN_CENTER, 0, -40);

    s_sub_label = lv_label_create(scr);
    lv_label_set_text(s_sub_label, "starting");
    lv_obj_set_style_text_font(s_sub_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sub_label, lv_color_hex(0xcccccc), 0);
    lv_obj_align(s_sub_label, LV_ALIGN_CENTER, 0, 10);

    s_log_label = lv_label_create(scr);
    lv_label_set_text(s_log_label, "");
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(0x667788), 0);
    lv_obj_set_style_text_align(s_log_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_log_label, LV_ALIGN_BOTTOM_MID, 0, -28);
}

/* Drain events for up to `wait_ms`; returns the most confident word heard or NULL. */
static const char *wait_for_word(uint32_t wait_ms, float *prob)
{
    word_event_t ev;
    const char *best = NULL;
    *prob = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
    while (xTaskGetTickCount() < end) {
        TickType_t left = end - xTaskGetTickCount();
        if (xQueueReceive(s_events, &ev, left) == pdTRUE && ev.prob > *prob) {
            best = ev.text;
            *prob = ev.prob;
        }
    }
    return best;
}

static void self_test(void)
{
    int pass = 0;
    char line[96];
    ui_show("self-test", "feeding synthesised clips");

    for (size_t i = 0; i < CLIP_COUNT; i++) {
        const clip_t *c = &s_clips[i];
        size_t samples = (size_t)(c->end - c->start) / sizeof(int16_t);
        xQueueReset(s_events);
        recognizer_inject((const int16_t *)c->start, samples, c->label);
        while (recognizer_injecting()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        float prob = 0;
        const char *heard = wait_for_word(1500, &prob);
        bool ok = heard != NULL && strcmp(heard, c->expect) == 0;
        pass += ok;
        ESP_LOGI(TAG, "self-test %u/%u: clip '%s' -> %s%s%s  [%s]", (unsigned)i + 1, (unsigned)CLIP_COUNT, c->label,
                 heard ? "'" : "", heard ? heard : "nothing", heard ? "'" : "", ok ? "PASS" : "FAIL");
        if (heard) {
            ESP_LOGI(TAG, "           prob=%.3f expected '%s'", prob, c->expect);
        }
        snprintf(line, sizeof(line), "clip %s -> %s", c->label, heard ? heard : "nothing");
        ui_log(line);
    }
    ESP_LOGI(TAG, "self-test: %d/%u clips recognised", pass, (unsigned)CLIP_COUNT);
    snprintf(line, sizeof(line), "self-test %d/%u", pass, (unsigned)CLIP_COUNT);
    ui_log(line);
}

void app_main(void)
{
    ESP_LOGI(TAG, "02_word_book_en M1 starting");
    s_events = xQueueCreate(8, sizeof(word_event_t));

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

    audio_io_t io = {0};
    if (audio_io_init(&io) != ESP_OK) {
        ui_show("audio failed", "see serial");
        return;
    }
    if (recognizer_start(io.mic, s_words, WORD_COUNT, s_events) != ESP_OK) {
        ui_show("recogniser failed", "see serial");
        return;
    }

    /* Let the AFE settle on room noise before the clips go in. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    self_test();

    ui_show("listening", "say a word");
    uint32_t last_beat = xTaskGetTickCount();
    for (;;) {
        word_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_heard++;
            char sub[48];
            snprintf(sub, sizeof(sub), "%.0f%%  (#%" PRIu32 ")", ev.prob * 100.0f, s_heard);
            ui_show(ev.text, sub);
            ESP_LOGI(TAG, "heard '%s' prob=%.3f", ev.text, ev.prob);
        }
        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive: heard=%" PRIu32 " internal=%u psram=%u", s_heard,
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}
