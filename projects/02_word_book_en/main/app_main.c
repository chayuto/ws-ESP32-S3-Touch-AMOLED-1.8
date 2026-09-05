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
#include "devcmd.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "maint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "photo.h"
#include "player.h"
#include "recognizer.h"
#include "sdcard.h"
#include "sdlog.h"
#include "timesync.h"
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
static audio_io_t s_io;
static book_t s_books[2];
static word_def_t s_defs[2][BOOK_MAX_WORDS];
static int s_active;
#define s_book (s_books[s_active])
static bool s_files_ok;   /* card present and the book's files reachable */
static void build_defs(int slot);
static void card_arrived(bool at_boot);
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

/* --- Maintenance: Wi-Fi + HTTP API, recogniser stopped. Long press or 'm' toggles. --- */

static bool s_maint;
static const char *s_last_word_text;
static float s_last_prob;
static void leave_sleep(void);
static void restart_recognizer(void);

static void maint_state(maint_app_state_t *out)
{
    out->heard = s_heard;
    out->last_word = s_last_word_text;
    out->last_prob = s_last_prob;
    out->words = (int)s_book.count;
    out->files_ok = s_files_ok;
}

static void enter_maint(void)
{
    if (s_maint) {
        return;
    }
    leave_sleep();
    cards_show_info("setup", "joining Wi-Fi...");
    cards_status("");
    recognizer_stop();
    if (maint_start(maint_state) != ESP_OK) {
        cards_show_info("setup failed", "no Wi-Fi - see serial");
        vTaskDelay(pdMS_TO_TICKS(2000));
        restart_recognizer();
        cards_show_idle();
        return;
    }
    s_maint = true;
    char line[64];
    snprintf(line, sizeof(line), "http://%s", maint_ip());
    cards_show_info("setup", line);
    cards_status("hold BOOT to finish");
}

static void leave_maint(void)
{
    if (!s_maint) {
        return;
    }
    s_maint = false;
    maint_stop();
    if (maint_take_book_changed() && sdcard_present()) {
        card_arrived(false);
    }
    restart_recognizer();
    wake_screen();
    cards_show_idle();
    cards_status("listening");
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


/* React to a word exactly as the child will see it. */
static void on_word(const word_event_t *ev)
{
    if (ev->id < 0 || (size_t)ev->id >= s_book.count) {
        return;
    }
    s_heard++;
    s_last_word = ev->id;
    s_last_word_text = s_book.words[ev->id].text;
    s_last_prob = ev->prob;
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

static void restart_recognizer(void)
{
    build_defs(s_active);
    if (recognizer_start(s_io.mic, s_defs[s_active], s_book.count, s_events) != ESP_OK) {
        ESP_LOGE(TAG, "recogniser did not restart");
        cards_status("recogniser failed - reboot");
    }
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
        s_active = slot;
        if (!s_maint) {
            recognizer_set_words(s_defs[slot], nb->count);
        }
        ESP_LOGI(TAG, "new book: %u words; vocabulary swap %s", (unsigned)nb->count, s_maint ? "on exit" : "requested");
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

/* Decode an embedded JPEG and show it: proves the photo path with no card at all. */
extern const uint8_t testjpg_start[] asm("_binary_baby_jpg_start");
extern const uint8_t testjpg_end[] asm("_binary_baby_jpg_end");

static void photo_self_test(void)
{
    uint8_t *buf = heap_caps_malloc(PHOTO_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return;
    }
    int w = 0, h = 0;
    if (photo_from_jpeg(testjpg_start, (size_t)(testjpg_end - testjpg_start), buf, &w, &h)) {
        ESP_LOGI(TAG, "photo self-test: embedded %dx%d JPEG decoded and shown  [PASS]", w, h);
        cards_show_buffer(buf, "photo test");
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGE(TAG, "photo self-test: decode failed  [FAIL]");
    }
    free(buf);
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
        /* Judge the engine's top guess, not the gated event: this checks the pipeline
         * end to end, while the floor is a tuning choice that changes with vocabulary size
         * (a synthesised "dog" scored 0.72 against 8 words and 0.19 against 10). */
        word_event_t ev;
        wait_for_word(1500, &ev);
        word_event_t raw;
        bool got = recognizer_take_raw(&raw);
        bool ok = got && strcmp(raw.text, c->expect) == 0;
        pass += ok;
        ESP_LOGI(TAG, "self-test %u/%u: clip '%s' -> %s%s%s prob=%.3f  [%s]%s", (unsigned)i + 1, (unsigned)CLIP_COUNT,
                 c->label, got ? "'" : "", got ? raw.text : "nothing", got ? "'" : "", got ? raw.prob : 0.0f,
                 ok ? "PASS" : "FAIL", (ok && raw.prob < CONFIG_WORDBOOK_MIN_PROB_PCT / 100.0f) ? " (below floor: no card)" : "");
        if (got && raw.prob >= CONFIG_WORDBOOK_MIN_PROB_PCT / 100.0f) {
            on_word(&raw); /* show the card and play the chime, as a real hearing would */
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
    /* Our own tags at DEBUG from the first line; third-party stays at the INFO default. */
    static const char *const own_tags[] = {"wordbook", "recog", "book", "cards", "photo", "player", "sdcard", "sdlog",
                                           "time", "rtc", "wifi", "maint", "button", "devcmd", "audio_io"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }
    ESP_LOGI(TAG, "02_word_book_en starting");
    bool safe_mode = crash_loop_guard();
    s_events = xQueueCreate(8, sizeof(word_event_t));
    s_tap = xSemaphoreCreateBinary();

    lv_display_t *disp = cards_display_start(); /* the i2c.master pull-up warning it logs is benign and stays visible */
    bsp_display_brightness_set(CONFIG_WORDBOOK_BRIGHTNESS);
    s_last_activity = xTaskGetTickCount();
    ESP_LOGI(TAG, "display up: %" PRId32 "x%" PRId32, lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));
    if (bsp_display_lock(1000)) {
        cards_init(on_tap);
        bsp_display_unlock();
    }
    button_init();
    devcmd_init();
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

    /* Clock first, so the log file header and FAT timestamps carry real time. */
    cards_status("setting the clock");
    timesync_at_boot();

    /* Content: the card's book if there is one, built-in vocabulary if not. */
    if (sdcard_init()) {
        card_arrived(true);
    }
    if (!s_book.count) {
        book_load(&s_book, "/nonexistent"); /* falls straight through to the built-in list */
    }
    build_defs(s_active); /* whichever book ended up active, the recogniser needs its table */

    if (audio_io_init(&s_io) != ESP_OK) {
        cards_status("audio init failed - see serial");
        return;
    }
    player_init(s_io.spk);
    if (recognizer_start(s_io.mic, s_defs[s_active], s_book.count, s_events) != ESP_OK) {
        cards_status("recogniser failed - see serial");
        return;
    }

#if CONFIG_WORDBOOK_BOOT_SELFTEST
    photo_self_test();
    vTaskDelay(pdMS_TO_TICKS(500));
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
        if (bev != BUTTON_NONE) {
            ESP_LOGI(TAG, "BOOT %s press", bev == BUTTON_LONG ? "long" : "short");
        }
        /* The card can come and go whether we are awake or asleep. */
        bool card_changed = sdcard_poll();
        char cmd = devcmd_take();
        if (bev == BUTTON_LONG || cmd == 'm') {
            if (s_maint) {
                leave_maint();
            } else {
                enter_maint();
            }
        } else if ((bev == BUTTON_SHORT || cmd == 's') && !s_maint) {
            if (s_asleep) {
                leave_sleep();
            } else {
                enter_sleep("button");
            }
        } else if (cmd == 'r' && sdcard_present()) {
            card_arrived(false);
        } else if (cmd == 'd') {
            static bool all_debug;
            all_debug = !all_debug;
            esp_log_level_set("*", all_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);
            ESP_LOGI(TAG, "log level for every tag: %s", all_debug ? "DEBUG" : "INFO (own tags stay DEBUG)");
            if (!all_debug) {
                for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
                    esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
                }
            }
        } else if (cmd == 'i') {
            char now[32];
            ESP_LOGI(TAG, "info: %s heard=%" PRIu32 " words=%u card=%d files=%d log=%ld B maint=%d asleep=%d internal=%u",
                     timesync_now_str(now, sizeof(now)), s_heard, (unsigned)s_book.count, sdcard_present(), s_files_ok,
                     sdlog_size(), s_maint, s_asleep, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
        if (s_maint) {
            xQueueReset(s_events);
            xSemaphoreTake(s_tap, 0);
            if (maint_take_reboot()) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            if (maint_idle_s() >= CONFIG_WORDBOOK_MAINT_IDLE_MIN * 60) {
                ESP_LOGI(TAG, "maintenance idle for %d min; leaving", CONFIG_WORDBOOK_MAINT_IDLE_MIN);
                leave_maint();
            }
            continue;
        }
        if (card_changed) {
            if (sdcard_present()) {
                card_arrived(false);
            } else {
                card_left();
            }
        }
        if (s_asleep) {
            xQueueReset(s_events);
            xSemaphoreTake(s_tap, 0);
            continue; /* nothing else runs while asleep; the loop still turns for the button and card */
        }
        if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
            on_tap_main();
        }
        if (xTaskGetTickCount() - s_last_activity >= pdMS_TO_TICKS(CONFIG_WORDBOOK_SLEEP_AFTER_S * 60 * 1000)) {
            enter_sleep("quiet");
            continue;
        }
        maybe_dim();
        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = xTaskGetTickCount();
            char now[32];
            ESP_LOGI(TAG, "alive: %s heard=%" PRIu32 " internal=%u psram=%u card=%d files=%d log=%d words=%u",
                     timesync_now_str(now, sizeof(now)), s_heard, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM), sdcard_present(), s_files_ok, sdlog_active(),
                     (unsigned)s_book.count);
        }
    }
}
