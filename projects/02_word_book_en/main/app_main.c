/*
 * 02_word_book_en — M2: the whole loop.
 *
 * SD card -> words.json -> vocabulary. A word is heard -> its photo (or a text
 * card) fills the screen, a chime plays, and if the book has a recorded prompt
 * for it, that plays too. With no card in the slot the built-in starter
 * vocabulary runs on text cards, so the loop is testable without content.
 *
 * The boot self-test from M1 stays: three synthesised clips through the
 * recogniser, so the engine is proven on every boot without anyone present.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "book.h"
#include "bsp/esp-bsp.h"
#include "cards.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "player.h"
#include "recognizer.h"
#include "sdkconfig.h"

static const char *TAG = "wordbook";

#define BOOK_DIR BSP_SD_MOUNT_POINT "/book"

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

static book_t s_book;
static word_def_t s_defs[BOOK_MAX_WORDS];
static QueueHandle_t s_events;
static uint32_t s_heard;
static int s_last_word = -1;
static TickType_t s_last_activity;
static bool s_dimmed;
static SemaphoreHandle_t s_tap;

/* --- Screen brightness: bright while in use, dim after a quiet spell -------- */

static void wake_screen(void)
{
    s_last_activity = xTaskGetTickCount();
    if (s_dimmed) {
        s_dimmed = false;
        bsp_display_brightness_set(CONFIG_WORDBOOK_BRIGHTNESS);
        ESP_LOGI(TAG, "screen: bright");
    }
}

static void maybe_dim(void)
{
    if (!s_dimmed && xTaskGetTickCount() - s_last_activity >= pdMS_TO_TICKS(CONFIG_WORDBOOK_DIM_AFTER_S * 1000)) {
        s_dimmed = true;
        bsp_display_brightness_set(CONFIG_WORDBOOK_DIM_BRIGHTNESS);
        ESP_LOGI(TAG, "screen: dim after %d s quiet", CONFIG_WORDBOOK_DIM_AFTER_S);
    }
}

/* From the LVGL task: just flag it, the main loop does the work. */
static void on_tap(void)
{
    xSemaphoreGive(s_tap);
}

/* React to a word exactly as the child will see it. */
static void on_word(const word_event_t *ev)
{
    if (ev->id < 0 || (size_t)ev->id >= s_book.count) {
        return;
    }
    const book_word_t *w = &s_book.words[ev->id];
    s_heard++;
    s_last_word = ev->id;
    ESP_LOGI(TAG, "heard '%s' prob=%.3f -> %s card", w->text, ev->prob, w->photo[0] ? "photo" : "text");
    wake_screen();
    cards_show_word(w, ev->prob, s_heard);
    player_chime();
    if (w->prompt[0]) {
        player_wav(w->prompt);
    }
}

/* Tap the picture: hear it again. Also the way to un-dim without saying anything. */
static void replay_last(void)
{
    wake_screen();
    if (s_last_word < 0) {
        ESP_LOGI(TAG, "tap: nothing to replay yet");
        return;
    }
    const book_word_t *w = &s_book.words[s_last_word];
    ESP_LOGI(TAG, "tap: replay '%s'", w->text);
    player_chime();
    if (w->prompt[0]) {
        player_wav(w->prompt);
    }
}

/* Drain events for up to `wait_ms`; returns the most confident one, or false. */
static bool wait_for_word(uint32_t wait_ms, word_event_t *best)
{
    word_event_t ev;
    bool any = false;
    best->prob = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
    while (xTaskGetTickCount() < end) {
        TickType_t left = end - xTaskGetTickCount();
        if (xQueueReceive(s_events, &ev, left) == pdTRUE && ev.prob > best->prob) {
            *best = ev;
            any = true;
        }
    }
    return any;
}

static void self_test(void)
{
    int pass = 0;
    char line[64];
    cards_status("self-test: feeding synthesised clips");

    for (size_t i = 0; i < CLIP_COUNT; i++) {
        const clip_t *c = &s_clips[i];
        size_t samples = (size_t)(c->end - c->start) / sizeof(int16_t);
        xQueueReset(s_events);
        recognizer_inject((const int16_t *)c->start, samples, c->label);
        while (recognizer_injecting()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        word_event_t ev;
        bool got = wait_for_word(1500, &ev);
        bool ok = got && strcmp(ev.text, c->expect) == 0;
        pass += ok;
        ESP_LOGI(TAG, "self-test %u/%u: clip '%s' -> %s%s%s prob=%.3f  [%s]", (unsigned)i + 1, (unsigned)CLIP_COUNT,
                 c->label, got ? "'" : "", got ? ev.text : "nothing", got ? "'" : "", got ? ev.prob : 0.0f,
                 ok ? "PASS" : "FAIL");
        if (got) {
            on_word(&ev); /* show the card and play the chime, as a real hearing would */
            vTaskDelay(pdMS_TO_TICKS(400)); /* let the pipeline settle after the pause */
        }
        snprintf(line, sizeof(line), "self-test %u/%u: %s -> %s", (unsigned)i + 1, (unsigned)CLIP_COUNT, c->label,
                 got ? ev.text : "nothing");
        cards_status(line);
    }
    ESP_LOGI(TAG, "self-test: %d/%u clips recognised", pass, (unsigned)CLIP_COUNT);
    snprintf(line, sizeof(line), "self-test %d/%u", pass, (unsigned)CLIP_COUNT);
    cards_status(line);
}

void app_main(void)
{
    ESP_LOGI(TAG, "02_word_book_en starting");
    s_events = xQueueCreate(8, sizeof(word_event_t));
    s_tap = xSemaphoreCreateBinary();

    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    lv_display_t *disp = bsp_display_start();
    esp_log_level_set("i2c.master", ESP_LOG_INFO);
    bsp_display_brightness_set(CONFIG_WORDBOOK_BRIGHTNESS);
    s_last_activity = xTaskGetTickCount();
    ESP_LOGI(TAG, "display up: %" PRId32 "x%" PRId32, lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));
    if (bsp_display_lock(1000)) {
        cards_init(on_tap);
        bsp_display_unlock();
    }
    cards_show_idle();
    cards_status("starting");

    /* Content: SD card if there is one, built-in vocabulary if not. */
    esp_err_t sd = bsp_sdcard_mount();
    if (sd == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s: %s, %llu MB", BSP_SD_MOUNT_POINT, bsp_sdcard->cid.name,
                 ((uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size) / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no SD card (%s); text cards only", esp_err_to_name(sd));
    }
    book_load(&s_book, BOOK_DIR);
    for (size_t i = 0; i < s_book.count; i++) {
        s_defs[i].text = s_book.words[i].text;
    }

    audio_io_t io = {0};
    if (audio_io_init(&io) != ESP_OK) {
        cards_status("audio init failed - see serial");
        return;
    }
    player_init(io.spk);
    if (recognizer_start(io.mic, s_defs, s_book.count, s_events) != ESP_OK) {
        cards_status("recogniser failed - see serial");
        return;
    }

#if CONFIG_WORDBOOK_BOOT_SELFTEST
    vTaskDelay(pdMS_TO_TICKS(1500));
    self_test();
#endif

    char status[48];
    snprintf(status, sizeof(status), "listening: %u words%s", (unsigned)s_book.count, s_book.from_sd ? " from SD" : ", built-in");
    cards_status(status);

    TickType_t last_beat = xTaskGetTickCount();
    for (;;) {
        word_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(250)) == pdTRUE) {
            on_word(&ev);
            xQueueReset(s_events); /* anything heard while the chime played was us */
        }
        if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
            replay_last();
        }
        maybe_dim();
        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive: heard=%" PRIu32 " internal=%u psram=%u", s_heard,
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}
