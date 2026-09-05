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
#include "button.h"
#include "bsp/esp-bsp.h"
#include "cards.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "player.h"
#include "recognizer.h"
#include "sdcard.h"
#include "sdlog.h"
#include "webui.h"
#include "sdkconfig.h"

static const char *TAG = "wordbook";

#define BOOK_DIR BSP_SD_MOUNT_POINT "/book"
#define LOG_PATH BSP_SD_MOUNT_POINT "/02_word_book_en.log"

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

/*
 * Two books and two word tables, used alternately. The recogniser reads its
 * table from another core, so a vocabulary swap builds the inactive pair in
 * full and hands it over; the previous pair stays untouched until the swap
 * after that, which is seconds away at the very least.
 */
static book_t s_books[2];
static word_def_t s_defs[2][BOOK_MAX_WORDS];
static int s_active;
#define s_book (s_books[s_active])
static bool s_files_ok;   /* card present and the book's files reachable */
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

/* --- Sleep: screen off, ears off. BOOT toggles it; quiet for long enough enters it. --- */

static bool s_asleep;

static void enter_sleep(const char *why)
{
    if (s_asleep) {
        return;
    }
    s_asleep = true;
    recognizer_pause();
    bsp_display_brightness_set(0);
    ESP_LOGI(TAG, "sleep (%s): screen off, not listening; press BOOT to wake", why);
}

static void leave_sleep(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    s_dimmed = true; /* so wake_screen() restores full brightness */
    wake_screen();
    recognizer_resume();
    ESP_LOGI(TAG, "awake: listening");
}

/* --- Setup mode: Wi-Fi + the drag-and-drop page. Long-press BOOT in and out. --- */

static bool s_setup;

static void enter_setup(void)
{
    if (s_setup) {
        return;
    }
    leave_sleep();
    recognizer_pause();
    if (webui_start() != ESP_OK) {
        cards_status("setup mode failed - see serial");
        recognizer_resume();
        return;
    }
    s_setup = true;
    char line[96];
    snprintf(line, sizeof(line), "Wi-Fi %s / %s\nhttp://192.168.4.1", CONFIG_WORDBOOK_WIFI_SSID, CONFIG_WORDBOOK_WIFI_PASSWORD);
    cards_show_setup(line);
    ESP_LOGI(TAG, "setup mode on");
}

static void leave_setup(void)
{
    if (!s_setup) {
        return;
    }
    s_setup = false;
    webui_stop();
    recognizer_resume();
    wake_screen();
    cards_show_idle();
    cards_status("listening");
    ESP_LOGI(TAG, "setup mode off");
}

/* React to a word exactly as the child will see it. */
static void on_word(const word_event_t *ev)
{
    if (ev->id < 0 || (size_t)ev->id >= s_book.count) {
        return;
    }
    s_heard++;
    s_last_word = ev->id;
    book_word_t w = s_book.words[ev->id];
    if (!s_files_ok) {
        /* No card, or it went away: the word still works, on a text card with the chime. */
        w.photo[0] = '\0';
        w.prompt[0] = '\0';
    }
    bool confident = ev->prob >= CONFIG_WORDBOOK_SOUND_PROB_PCT / 100.0f;
    ESP_LOGI(TAG, "heard '%s' prob=%.3f -> %s card, %s", w.text, ev->prob, w.photo[0] ? "photo" : "text",
             confident ? "with sound" : "silent (below sound threshold)");
    wake_screen();
    if (!cards_show_word(&w, ev->prob, s_heard) && w.photo[0]) {
        sdcard_report_io_error();
    }
    if (!confident) {
        return;
    }
    player_chime();
    if (w.prompt[0] && player_wav(w.prompt) != ESP_OK) {
        sdcard_report_io_error();
    }
}

/* Tap: wake the screen if it has dimmed. Nothing else; taps make no sound. */
static void on_tap_main(void)
{
    ESP_LOGI(TAG, "tap");
    wake_screen();
}

/* Build the word table the recogniser reads from a book. */
static void build_defs(int slot)
{
    for (size_t i = 0; i < s_books[slot].count; i++) {
        s_defs[slot][i].text = s_books[slot].words[i].text;
    }
}

/* The card appeared (at boot, or later): start the log file, take its book. */
static void card_arrived(bool at_boot)
{
    sdlog_open(LOG_PATH);
    int slot = at_boot ? s_active : s_active ^ 1;
    book_t *nb = &s_books[slot];
    book_load(nb, BOOK_DIR);
    if (!nb->from_sd) {
        /* At boot the fallback list has just been loaded into the active slot; the caller
         * builds its word table. Later on, the current words simply stay. */
        ESP_LOGW(TAG, "card present but no usable %s/words.json; %s", BOOK_DIR,
                 at_boot ? "using built-in words" : "keeping current words");
        s_files_ok = false;
        return;
    }
    if (at_boot) {
        /* caller builds the table */
    } else if (book_same_words(nb, &s_book)) {
        book_adopt_files(&s_book, nb);
        ESP_LOGI(TAG, "same %u words as before; photos and prompts now available", (unsigned)s_book.count);
    } else {
        build_defs(slot);
        recognizer_set_words(s_defs[slot], nb->count);
        s_active = slot;
        ESP_LOGI(TAG, "new book: %u words; vocabulary swap requested", (unsigned)nb->count);
    }
    s_files_ok = true;
}

static void card_left(void)
{
    sdlog_close();
    s_files_ok = false;
    ESP_LOGW(TAG, "card lost; keeping %u words, text cards until it returns", (unsigned)s_book.count);
    cards_status("card removed - text cards");
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

/*
 * Crash-loop guard. A firmware bug that panics at boot reboots the chip every
 * few seconds; on this board that has taken the USB console down with it and
 * cost a manual power cycle. After three consecutive panics, stop before the
 * SD card and the recogniser, show why on screen, and just keep the console
 * alive so the next flash can go in.
 */
static RTC_NOINIT_ATTR uint32_t s_panic_streak;
static RTC_NOINIT_ATTR uint32_t s_panic_magic;
#define PANIC_MAGIC 0x50414e43u /* "PANC" */
#define PANIC_LIMIT 3

static bool crash_loop_guard(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (s_panic_magic != PANIC_MAGIC) {
        s_panic_magic = PANIC_MAGIC;
        s_panic_streak = 0;
    }
    if (why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT || why == ESP_RST_WDT) {
        s_panic_streak++;
        ESP_LOGW(TAG, "reset reason %d, consecutive crash %" PRIu32 "/%d", (int)why, s_panic_streak, PANIC_LIMIT);
    } else {
        s_panic_streak = 0;
    }
    return s_panic_streak >= PANIC_LIMIT;
}

void app_main(void)
{
    sdlog_init(); /* first, so every line below is in the ring before a card is even mounted */
    ESP_LOGI(TAG, "02_word_book_en starting");
    bool safe_mode = crash_loop_guard();
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
    button_init();
    cards_show_idle();
    cards_status("starting");

    if (safe_mode) {
        ESP_LOGE(TAG, "SAFE MODE: %d crashes in a row. Not touching SD or the recogniser. Flash a fix.", PANIC_LIMIT);
        cards_status("safe mode: crashed 3x - flash a fix");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGE(TAG, "safe mode, waiting for a flash (streak %" PRIu32 ")", s_panic_streak);
        }
    }

    /* Content: the card's book if there is one, built-in vocabulary if not. */
    if (sdcard_init()) {
        card_arrived(true);
    }
    if (!s_book.count) {
        book_load(&s_book, "/nonexistent"); /* falls straight through to the built-in list */
    }
    build_defs(s_active); /* whichever book ended up active, the recogniser needs its table */

    audio_io_t io = {0};
    if (audio_io_init(&io) != ESP_OK) {
        cards_status("audio init failed - see serial");
        return;
    }
    player_init(io.spk);
    if (recognizer_start(io.mic, s_defs[s_active], s_book.count, s_events) != ESP_OK) {
        cards_status("recogniser failed - see serial");
        return;
    }

#if CONFIG_WORDBOOK_BOOT_SELFTEST
    vTaskDelay(pdMS_TO_TICKS(1500));
    self_test();
#endif

    s_panic_streak = 0; /* made it through start-up: a later crash starts a new count */

    char status[48];
    snprintf(status, sizeof(status), "listening: %u words%s", (unsigned)s_book.count, s_files_ok ? " from SD" : ", built-in");
    cards_status(status);

    TickType_t last_beat = xTaskGetTickCount();
    TickType_t last_word_tick = 0;
    for (;;) {
        word_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(250)) == pdTRUE) {
            /* One utterance, one card: the engine can fire twice on a word's tail. */
            if (last_word_tick && xTaskGetTickCount() - last_word_tick < pdMS_TO_TICKS(1000)) {
                ESP_LOGI(TAG, "ignored '%s' %.0f%%: within 1 s of the last word", ev.text, ev.prob * 100);
            } else {
                on_word(&ev);
                last_word_tick = xTaskGetTickCount();
            }
            xQueueReset(s_events); /* anything heard while the chime played was us */
        }
        button_event_t bev = button_poll();
        if (bev == BUTTON_LONG) {
            if (s_setup) {
                leave_setup();
            } else {
                enter_setup();
            }
        } else if (bev == BUTTON_SHORT && !s_setup) {
            if (s_asleep) {
                leave_sleep();
            } else {
                enter_sleep("button");
            }
        }
        if (s_setup) {
            xQueueReset(s_events);
            xSemaphoreTake(s_tap, 0);
            if (webui_take_book_changed() && sdcard_present()) {
                card_arrived(false); /* same path as a freshly inserted card */
            }
            if (sdcard_poll() && !sdcard_present()) {
                card_left();
            }
            continue;
        }
        if (s_asleep) {
            xQueueReset(s_events);
            xSemaphoreTake(s_tap, 0);
            continue; /* nothing else runs while asleep; the loop still turns for the button */
        }
        if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
            on_tap_main();
        }
        if (xTaskGetTickCount() - s_last_activity >= pdMS_TO_TICKS(CONFIG_WORDBOOK_SLEEP_AFTER_S * 60 * 1000)) {
            enter_sleep("quiet");
            continue;
        }
        if (sdcard_poll()) {
            if (sdcard_present()) {
                card_arrived(false);
            } else {
                card_left();
            }
        }
        maybe_dim();
        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive: heard=%" PRIu32 " internal=%u psram=%u", s_heard,
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}
