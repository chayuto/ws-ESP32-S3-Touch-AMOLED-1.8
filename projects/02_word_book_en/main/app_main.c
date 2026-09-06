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
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "book.h"
#include "button.h"
#include "bsp/esp-bsp.h"
#include "cards.h"
#include "devcmd.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "maint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "photo.h"
#include "player.h"
#include "pmu.h"
#include "recognizer.h"
#include "sdcard.h"
#include "sdlog.h"
#include "thermal.h"
#include "timesync.h"
#include "wifi_sta.h"
#include "sdkconfig.h"

static const char *TAG = "wordbook";

/* A Kconfig bool that is off has no #define at all. */
#ifdef CONFIG_WORDBOOK_SOUND
#define SOUND_ON 1
#else
#define SOUND_ON 0
#endif

#define BOOK_DIR BSP_SD_MOUNT_POINT "/book"
#define LOG_PATH BSP_SD_MOUNT_POINT "/02_word_book_en.log"
#define CLOG_PATH BSP_SD_MOUNT_POINT "/02_word_book_en.classifier.jsonl"
#define SLOG_PATH BSP_SD_MOUNT_POINT "/02_word_book_en.state.jsonl"

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
static const char *s_last_word_text;
static float s_last_prob;
static bool s_dimmed;
static book_t s_books[2];
static word_def_t s_defs[2][BOOK_MAX_WORDS];
static int s_active;
#define s_book (s_books[s_active])
static bool s_files_ok;   /* card present and the book's files reachable */
static void build_defs(int slot);
static void card_arrived(bool at_boot);
/*
 * One utterance, one card: the engine can fire twice on a word's tail. 1000 ms
 * threw away CAT at 0.70 arriving a second after BALL at 0.38 (2026-09-06), so the
 * window is now 700 ms.
 */
#define DEDUPE_MS 700

static QueueHandle_t s_events;
static uint32_t s_heard;
static int s_last_word = -1;
static TickType_t s_last_activity;
static SemaphoreHandle_t s_tap;

/* The thermal guard's last sample; thermal_step() refreshes it every loop turn. */
static thermal_status_t s_thermal;

/* Main-loop health: the longest turn's work (wait excluded) since the last state record,
 * and what that turn was doing, because "387 ms" on its own answers nothing. */
static int64_t s_loop_work_start, s_loop_max_us;
static uint32_t s_loop_turns;
static const char *s_loop_label = "idle", *s_loop_max_label = "idle";

/* Stack headroom of the tasks that matter, as one JSON object; -1 = not running. */
static const struct { const char *task, *key; } s_stack_tasks[] = {
    {"main", "main"},     {"sr_feed", "feed"}, {"sr_detect", "detect"}, {"taskLVGL", "lvgl"},
    {"sdlog", "sdlog"},   {"devcmd", "devcmd"}, {"esp_timer", "timer"},
};

static void stack_json(char *buf, size_t n)
{
    size_t len = 0;
    len += snprintf(buf + len, n - len, "{");
    for (size_t i = 0; i < sizeof(s_stack_tasks) / sizeof(s_stack_tasks[0]) && len < n; i++) {
        TaskHandle_t h = xTaskGetHandle(s_stack_tasks[i].task);
        len += snprintf(buf + len, n - len, "%s\"%s\":%d", i ? "," : "", s_stack_tasks[i].key,
                        h ? (int)uxTaskGetStackHighWaterMark(h) : -1);
    }
    if (len < n) {
        snprintf(buf + len, n - len, "}");
    }
}

/* A reading that is not there prints as -999, which no die ever reads; "nan" is not JSON. */
static float jnum(float v)
{
    return isnan(v) ? -999.0f : v;
}

/* --- Screen brightness: bright while in use, dim after a quiet spell -------- */

static TickType_t s_woke_at;

static void wake_screen(void)
{
    s_last_activity = xTaskGetTickCount();
    s_woke_at = s_last_activity;
    if (s_dimmed) {
        if (s_thermal.level >= THERMAL_WARM) {
            bsp_display_brightness_set(CONFIG_WORDBOOK_DIM_BRIGHTNESS); /* warm: dim is the ceiling, however often it is touched */
            return;
        }
        s_dimmed = false;
        esp_err_t err = bsp_display_brightness_set(CONFIG_WORDBOOK_BRIGHTNESS);
        ESP_LOGI(TAG, "screen: bright (%d%%, write %s)", CONFIG_WORDBOOK_BRIGHTNESS, esp_err_to_name(err));
    }
}

static void maybe_dim(void)
{
    if (!s_dimmed && xTaskGetTickCount() - s_last_activity >= pdMS_TO_TICKS(CONFIG_WORDBOOK_DIM_AFTER_S * 1000)) {
        s_loop_label = "dim";
        s_dimmed = true;
        esp_err_t err = bsp_display_brightness_set(CONFIG_WORDBOOK_DIM_BRIGHTNESS);
        ESP_LOGI(TAG, "screen: dim %d%% after %d s quiet (write %s)", CONFIG_WORDBOOK_DIM_BRIGHTNESS, CONFIG_WORDBOOK_DIM_AFTER_S,
                 esp_err_to_name(err));
    }
}

/* From the LVGL task: note where, flag it, the main loop does the work. */
static volatile int16_t s_tap_x = -1, s_tap_y = -1;

static void on_tap(int16_t x, int16_t y)
{
    s_tap_x = x;
    s_tap_y = y;
    xSemaphoreGive(s_tap);
}

/* --- The state record: everything the board knows about itself, one JSON line. --- */

static bool s_maint;
static bool s_asleep;

static void state_record(const char *event)
{
    if (s_loop_label == NULL || s_loop_label[0] == 'i') {
        s_loop_label = "record"; /* an otherwise idle turn that stopped to write a record */
    }
    pmu_status_t ps = {0};
    pmu_read(&ps);
    uint8_t dc = 0, l0 = 0, l1 = 0;
    pmu_rail_bits(&dc, &l0, &l1);
    float mic_avg = 0, mic_peak = 0;
    int speech = 0;
    recognizer_mic_level(&mic_avg, &mic_peak, &speech);
    static const char *const chg_names[] = {"tri", "pre", "cc", "cv", "done", "idle", "?", "?"};
    /* Health: the audio path, the loop, the heap's shape, the tasks' stacks, the card and the log. */
    recognizer_health_t h;
    recognizer_health(&h, true);
    unsigned loop_max_ms = (unsigned)(s_loop_max_us / 1000);
    const char *loop_max_what = s_loop_max_label;
    s_loop_max_us = 0;
    s_loop_max_label = "idle";
    char stacks[160];
    stack_json(stacks, sizeof(stacks));
    uint8_t irq[3];
    pmu_irq_status(irq);
    uint32_t card_mb = 0, card_free_mb = 0;
    sdcard_space(&card_mb, &card_free_mb);
    int rssi = 0, chan = 0;
    wifi_sta_signal(&rssi, &chan);
    char now[32], buf[1280];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"state\",\"time\":\"%s\",\"up_s\":%lld,\"event\":\"%s\","
             "\"usb\":%d,\"vbus_mv\":%u,\"battery\":%d,\"vbat_mv\":%u,\"pct\":%u,\"vsys_mv\":%u,\"charging\":%d,\"chg\":\"%s\","
             "\"dcdc_en\":\"%02x\",\"ldo_en\":\"%02x%02x\","
             "\"screen\":\"%s\",\"brightness\":%d,\"asleep\":%d,\"maint\":%d,"
             "\"internal\":%u,\"internal_min\":%u,\"psram\":%u,"
             "\"card\":%d,\"files\":%d,\"log\":%d,\"log_bytes\":%ld,"
             "\"words\":%u,\"heard\":%lu,\"last\":\"%s\",\"last_p\":%.2f,"
             "\"chip_c\":%.1f,\"pmu_c\":%.1f,\"thermal\":\"%s\","
             "\"mic_avg\":%.1f,\"mic_peak\":%.1f,\"speech_pct\":%d,"
             "\"host\":%d,\"pmu_irq\":\"%02x%02x%02x\",\"internal_largest\":%u,\"psram_min\":%u,"
             "\"card_mb\":%" PRIu32 ",\"card_free_mb\":%" PRIu32 ",\"log_dropped\":%u,\"io_errors\":%" PRIu32 ","
             "\"state_bytes\":%ld,\"clog_bytes\":%ld,\"loop_max_ms\":%u,\"loop_max_what\":\"%s\",\"loop_turns\":%" PRIu32 ","
             "\"afe\":{\"frames\":%" PRIu32 ",\"timeouts\":%" PRIu32 ",\"mic_err\":%" PRIu32 ",\"q_drops\":%" PRIu32 ","
             "\"rb_min\":%.2f,\"rb_max\":%.2f,\"gap_max_ms\":%" PRIu32 ",\"detect_max_ms\":%" PRIu32 "},"
             "\"stack\":%s,\"rssi\":%d,\"chan\":%d,\"join_ms\":%d}",
             timesync_now_str(now, sizeof(now)), (long long)(esp_timer_get_time() / 1000000), event,
             ps.vbus_in, ps.vbus_mv, ps.batt_present, ps.vbat_mv, ps.batt_pct, ps.vsys_mv, ps.charging, chg_names[ps.chg_state & 7],
             dc, l0, l1,
             s_asleep ? "off" : s_dimmed ? "dim" : "on",
             s_asleep ? 0 : s_dimmed ? CONFIG_WORDBOOK_DIM_BRIGHTNESS : CONFIG_WORDBOOK_BRIGHTNESS, s_asleep, s_maint,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             sdcard_present(), s_files_ok, sdlog_active(), sdlog_size(),
             (unsigned)s_book.count, (unsigned long)s_heard, s_last_word_text ? s_last_word_text : "", s_last_prob,
             jnum(s_thermal.chip_c), jnum(s_thermal.pmu_c), thermal_level_name(s_thermal.level), mic_avg, mic_peak, speech,
             usb_serial_jtag_is_connected(), irq[0], irq[1], irq[2], (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), card_mb, card_free_mb, (unsigned)sdlog_dropped(),
             sdcard_io_errors(), sdlog_aux_size(1), sdlog_aux_size(0), loop_max_ms, loop_max_what, s_loop_turns, h.frames, h.fetch_timeouts,
             h.mic_errors, h.queue_drops, h.rb_min, h.rb_max, h.gap_max_ms, h.detect_max_ms, stacks, rssi, chan,
             wifi_sta_join_ms());
    sdlog_aux_write(1, buf);
}

/* --- Every press, tap and serial command: what it landed on and what it did. --- */

static const char *screen_name(void)
{
    return s_asleep ? "off" : s_dimmed ? "dim" : "on";
}

static uint32_t since_wake_s(void)
{
    return (uint32_t)((xTaskGetTickCount() - s_woke_at) / configTICK_RATE_HZ);
}

/*
 * One line in the log and one `input` record in the state file per interaction, taken
 * before the action changes anything: the screen it landed on, whether the board was
 * asleep or in maintenance, seconds since the last wake, and the outcome. The "screen
 * went dark" question is then answered by one line instead of a log read.
 */
static void input_record(const char *src, const char *kind, uint32_t held_ms, int x, int y, const char *screen_before,
                         bool asleep_before, bool maint_before, uint32_t wake_s, const char *action)
{
    char detail[40] = "";
    if (strcmp(src, "boot") == 0) {
        snprintf(detail, sizeof(detail), ", held %lu ms", (unsigned long)held_ms);
    } else if (strcmp(src, "touch") == 0) {
        snprintf(detail, sizeof(detail), " at %d,%d", x, y);
    }
    ESP_LOGI(TAG, "input: %s %s%s, screen %s%s%s, %lu s since wake -> %s", src, kind, detail, screen_before,
             asleep_before ? " (asleep)" : "", maint_before ? " (maintenance)" : "", (unsigned long)wake_s, action);
    char now[32], buf[320];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"input\",\"time\":\"%s\",\"up_s\":%lld,\"src\":\"%s\",\"kind\":\"%s\",\"held_ms\":%lu,"
             "\"x\":%d,\"y\":%d,\"screen\":\"%s\",\"asleep\":%d,\"maint\":%d,\"since_wake_s\":%lu,\"action\":\"%s\"}",
             timesync_now_str(now, sizeof(now)), (long long)(esp_timer_get_time() / 1000000), src, kind,
             (unsigned long)held_ms, x, y, screen_before, asleep_before, maint_before, (unsigned long)wake_s, action);
    sdlog_aux_write(1, buf);
}

static void tap_record(const char *action)
{
    input_record("touch", "tap", 0, s_tap_x, s_tap_y, screen_name(), s_asleep, s_maint, since_wake_s(), action);
}

/* --- Maintenance: Wi-Fi + HTTP API, recogniser stopped. Long press or 'm' toggles. --- */

static void leave_sleep(bool listen);
static void restart_recognizer(void);
static void normal_status(void);
static void boot_record(void);

static void maint_state(maint_app_state_t *out)
{
    out->heard = s_heard;
    out->last_word = s_last_word_text;
    out->last_prob = s_last_prob;
    out->words = (int)s_book.count;
    out->files_ok = s_files_ok;
    out->loop_max_ms = (uint32_t)(s_loop_max_us / 1000);
    out->loop_turns = s_loop_turns;
    stack_json(out->stack_json, sizeof(out->stack_json));
}

static void enter_maint(void)
{
    if (s_maint) {
        return;
    }
    leave_sleep(false); /* screen on; the recogniser stays stopped, Wi-Fi needs the room */
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
    state_record("maint_in");
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
    normal_status();
    state_record("maint_out");
}

/* --- Sleep: screen off, ears off. The quiet timer or the serial `s` enters it; BOOT leaves it. --- */

static void enter_sleep(const char *why)
{
    if (s_asleep) {
        return;
    }
    s_asleep = true;
    /* Stop, not pause. A paused front end still ran the VAD on silence at full clock:
     * 2026-09-06, 72 minutes "asleep" on battery took the gauge from 95 % to 46 %, the
     * same drain as in use. Stopping frees the AFE as well; waking rebuilds it. */
    recognizer_stop();
    bsp_display_brightness_set(0);
    pmu_set_record_period_s(CONFIG_WORDBOOK_SLEEP_POWER_LOG_S);
    ESP_LOGI(TAG, "sleep (%s): screen off, recogniser stopped; press BOOT to wake", why);
    state_record("sleep");
}

/* `listen` false: the screen comes on but the recogniser stays stopped (maintenance). */
static void leave_sleep(bool listen)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    s_dimmed = true; /* so wake_screen() restores full brightness */
    wake_screen();
    pmu_set_record_period_s(CONFIG_WORDBOOK_POWER_LOG_S);
    if (listen) {
        int64_t t0 = esp_timer_get_time();
        restart_recognizer();
        ESP_LOGI(TAG, "awake: listening again, front end rebuilt in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    }
    state_record("wake");
}

/* --- Every press says something on the status line for ACK_MS; then the usual line returns. --- */

#define ACK_MS 2000
static TickType_t s_ack_until;

static void show_ack(const char *text)
{
    cards_status(text);
    s_ack_until = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_MS);
    if (s_ack_until == 0) {
        s_ack_until = 1;
    }
}

static void normal_status(void)
{
    char status[48];
    snprintf(status, sizeof(status), "listening: %u words%s", (unsigned)s_book.count, s_files_ok ? " from SD" : ", built-in");
    cards_status(status);
}

/* --- Thermal guard: the board must never be too hot for a child's hands. ---------------
 * thermal.c reads both dies once a second and decides a level; this is what the app does
 * about it. warm: the screen is held dim and the status line says so. hot: sleep (screen
 * off, recogniser stopped), charger off, and BOOT only shows a dim "cooling down" note.
 * trip: marker to NVS, record to the card, three seconds for the card, PMU soft power-off.
 * Levels drop with hysteresis; leaving hot turns the charger back on and leaves the board
 * asleep, so BOOT wakes it like any other sleep. Serial `t` simulates the levels; a
 * simulated trip is a dry run. */

static TickType_t s_hot_ack_until;
static int64_t s_trip_failed_us;

static void hot_ack(void)
{
    cards_status("too warm - cooling down, wait");
    bsp_display_brightness_set(CONFIG_WORDBOOK_DIM_BRIGHTNESS);
    s_hot_ack_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    if (s_hot_ack_until == 0) {
        s_hot_ack_until = 1;
    }
}

static void thermal_trip_now(const thermal_status_t *st)
{
    ESP_LOGE(TAG, "THERMAL TRIP: chip %.1f C, PMU %.1f C, line %d C%s", st->chip_c, st->pmu_c, CONFIG_WORDBOOK_THERMAL_TRIP_C,
             st->simulated ? " (SIMULATED: nothing written, nothing switched off)" : "");
    if (st->simulated) {
        return;
    }
    pmu_set_charging(false);
    if (!s_asleep) {
        enter_sleep("trip");
    }
    thermal_mark_trip(st);
    state_record("thermal_trip");
    vTaskDelay(pdMS_TO_TICKS(3000)); /* the drain task syncs the card every two seconds */
    if (pmu_power_off() != ESP_OK) {
        s_trip_failed_us = esp_timer_get_time();
        ESP_LOGE(TAG, "the PMU did not power off; asleep with the charger off, trying again every minute");
    }
}

static void thermal_step(void)
{
    thermal_status_t st;
    bool changed = thermal_poll(&st);
    thermal_level_t was = s_thermal.level;
    s_thermal = st;
    if (!changed) {
        if (st.level == THERMAL_TRIP && !st.simulated && s_trip_failed_us &&
            esp_timer_get_time() - s_trip_failed_us > 60 * 1000000LL) {
            thermal_trip_now(&st);
        }
        return;
    }
    ESP_LOGW(TAG, "thermal: %s -> %s (chip %.1f C, PMU %.1f C%s)", thermal_level_name(was), thermal_level_name(st.level),
             st.chip_c, st.pmu_c, st.simulated ? ", SIMULATED" : "");
    if (st.level >= THERMAL_HOT && was < THERMAL_HOT) {
        if (s_maint) {
            leave_maint(); /* the radio is heat too */
        }
        pmu_set_charging(false);
        if (!s_asleep) {
            cards_status("too warm - cooling down");
            enter_sleep("hot");
        }
    } else if (st.level < THERMAL_HOT && was >= THERMAL_HOT) {
        pmu_set_charging(true); /* still asleep; BOOT wakes it as usual */
    } else if (st.level == THERMAL_WARM && was == THERMAL_OK && !s_asleep && !s_maint) {
        if (!s_dimmed) {
            s_dimmed = true;
            bsp_display_brightness_set(CONFIG_WORDBOOK_DIM_BRIGHTNESS);
        }
        cards_status("warm - screen dimmed to cool");
    } else if (st.level == THERMAL_OK && was == THERMAL_WARM && !s_asleep && !s_maint) {
        normal_status();
    }
    char event[24];
    snprintf(event, sizeof(event), "thermal_%s", thermal_level_name(st.level));
    state_record(event);
    if (st.level == THERMAL_TRIP) {
        thermal_trip_now(&st);
    }
}

/* At boot, a board still hot from the last run goes straight back off, before the
 * screen and the recogniser add to it. Two readings a second apart must both agree. */
static void thermal_boot_check(void)
{
    thermal_status(&s_thermal);
    if (isnan(s_thermal.board_c) || s_thermal.board_c < CONFIG_WORDBOOK_THERMAL_TRIP_C) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1100));
    thermal_poll(&s_thermal);
    if (s_thermal.board_c >= CONFIG_WORDBOOK_THERMAL_TRIP_C) {
        ESP_LOGE(TAG, "%.1f C at boot", s_thermal.board_c);
        thermal_trip_now(&s_thermal);
    }
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
             !SOUND_ON ? "silent (sound off)" : confident ? "with sound" : "silent (below sound threshold)");
    wake_screen();
    if (!cards_show_word(&w, ev->prob, s_heard) && w.photo[0]) {
        sdcard_report_io_error();
    }
    if (!confident || !SOUND_ON) {
        return;
    }
    player_chime();
    if (w.prompt[0] && player_wav(w.prompt) != ESP_OK) {
        sdcard_report_io_error();
    }
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
    sdlog_aux_open(0, CLOG_PATH);
    sdlog_aux_open(1, SLOG_PATH);
    state_record(at_boot ? "boot" : "card_in");
    if (at_boot) {
        boot_record();
    }
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
    sdlog_aux_close(0);
    sdlog_aux_close(1);
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
        word_event_t ev = {0};
        wait_for_word(1500, &ev);
        word_event_t raw = {0};
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
                 got ? raw.text : "nothing"); /* raw, not ev: ev is only filled when a gated event arrived */
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
static bool s_cold_boot; /* RTC RAM had no magic: the chip had no power at all, not merely a reset */

static bool crash_loop_guard(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (s_panic_magic != PANIC_MAGIC) {
        s_cold_boot = true;
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

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

/*
 * One `boot` record per run, after the first state record: why the chip reset and, from
 * the PMU, why it last powered on and off. 2026-09-06: "did it lose power?" had no answer
 * on the card. Now it is one line.
 */
static void boot_record(void)
{
    uint8_t on = 0, off = 0;
    char on_txt[80], off_txt[80];
    pmu_power_sources(&on, &off, on_txt, sizeof(on_txt), off_txt, sizeof(off_txt));
    const char *why = reset_name(esp_reset_reason());
    /* The PMU's two registers hold their last cause across chip resets. RTC RAM says
     * whether this reset went through the RTC domain: "kept" means a warm reset with the
     * PMU on throughout; "lost" means the board had no power, or a full chip reset such
     * as the one esptool issues after a flash (which also reports "poweron"). */
    const char *rtc = s_cold_boot ? "lost" : "kept";
    ESP_LOGI(TAG, "boot: chip reset %s, RTC RAM %s%s; PMU last powered on by %s, last powered off by %s; crash streak %" PRIu32,
             why, rtc, s_cold_boot ? " (no power, or a full reset like esptool's)" : " (warm reset, PMU stayed on)", on_txt,
             off_txt, s_panic_streak);
    /* The charger as configured, and the last thermal trip this board ever made. */
    char chg_txt[120], trip_txt[160] = "null";
    pmu_charger_text(chg_txt, sizeof(chg_txt));
    thermal_trip_t trip = {0};
    if (thermal_last_trip(&trip)) {
        snprintf(trip_txt, sizeof(trip_txt), "{\"when\":%lld,\"up_s\":%" PRIu32 ",\"chip_c\":%.1f,\"pmu_c\":%.1f}",
                 (long long)trip.when, trip.up_s, jnum(trip.chip_c), jnum(trip.pmu_c));
    }
    ESP_LOGI(TAG, "boot: chip %.1f C, PMU %.1f C; %s; thermal trips ever %" PRIu32 "%s", s_thermal.chip_c, s_thermal.pmu_c, chg_txt,
             trip.count, off & 0x02 ? " (the PMU says the last power-off was by software: our trip)" : "");
    /* The clock's story, the PMU's latched history and the card's room. */
    timesync_info_t ti;
    timesync_info(&ti);
    char drift[16] = "null";
    if (ti.drift_known) {
        snprintf(drift, sizeof(drift), "%d", ti.rtc_drift_s);
    }
    uint8_t irq[3];
    pmu_irq_status(irq);
    int warn1 = 0, warn2 = 0;
    pmu_low_batt_levels(&warn1, &warn2);
    int ts_raw = pmu_ts_raw();
    uint32_t card_mb = 0, card_free_mb = 0;
    sdcard_space(&card_mb, &card_free_mb);
    ESP_LOGI(TAG, "boot: clock from %s, RTC %s, drift %s s, NTP path %d ms; PMU irq %02x%02x%02x, TS raw %d, low-battery warnings at %d%% and %d%%; card %" PRIu32 " of %" PRIu32 " MB free",
             ti.clock, ti.rtc_valid ? "valid" : "had STOPPED", drift, ti.ntp_ms, irq[0], irq[1], irq[2], ts_raw, warn1, warn2,
             card_free_mb, card_mb);
    char now[32], buf[960];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"boot\",\"time\":\"%s\",\"up_s\":%lld,\"reset\":\"%s\",\"rtc_ram\":\"%s\",\"pmu_on\":\"%02x\","
             "\"pmu_on_src\":\"%s\",\"pmu_off\":\"%02x\",\"pmu_off_src\":\"%s\",\"crash_streak\":%" PRIu32 ","
             "\"chip_c\":%.1f,\"pmu_c\":%.1f,\"chg_ma\":%d,\"chg_mv\":%d,\"in_ma\":%d,\"treg_c\":%d,"
             "\"trips\":%" PRIu32 ",\"last_trip\":%s,"
             "\"rtc_valid\":%d,\"clock\":\"%s\",\"rtc_drift_s\":%s,\"ntp_ms\":%d,\"ts_raw\":%d,\"warn_pct\":[%d,%d],"
             "\"pmu_irq\":\"%02x%02x%02x\",\"card_mb\":%" PRIu32 ",\"card_free_mb\":%" PRIu32 "}",
             timesync_now_str(now, sizeof(now)), (long long)(esp_timer_get_time() / 1000000), why, rtc, on, on_txt, off, off_txt,
             s_panic_streak, jnum(s_thermal.chip_c), jnum(s_thermal.pmu_c), pmu_charge_ma(), pmu_charge_target_mv(),
             pmu_input_limit_ma(), pmu_thermal_reg_c(), trip.count, trip_txt, ti.rtc_valid, ti.clock, drift, ti.ntp_ms, ts_raw,
             warn1, warn2, irq[0], irq[1], irq[2], card_mb, card_free_mb);
    sdlog_aux_write(1, buf);
}

void app_main(void)
{
    sdlog_init(); /* first, so every line below is in the ring before a card is even mounted */
    /* Our own tags at DEBUG from the first line; third-party stays at the INFO default. */
    static const char *const own_tags[] = {"wordbook", "recog", "book", "cards", "photo", "player", "sdcard", "sdlog",
                                           "time", "rtc", "wifi", "maint", "button", "devcmd", "audio_io", "pmu", "thermal"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }
    ESP_LOGI(TAG, "02_word_book_en starting");
    bool safe_mode = crash_loop_guard();
    s_events = xQueueCreate(8, sizeof(word_event_t));
    s_tap = xSemaphoreCreateBinary();

    lv_display_t *disp = cards_display_start(); /* the i2c.master pull-up warning it logs is benign and stays visible */
    if (disp == NULL) {
        ESP_LOGE(TAG, "DISPLAY INIT FAILED - running blind; audio and recogniser continue");
    }
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

    /* Power rails before anything that needs them; also tells us USB vs battery. */
    if (pmu_init() != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not answering; rails left as found");
    }
    pmu_set_record_cb(state_record);
    thermal_init();
    thermal_boot_check(); /* a board still too hot from the last run goes straight back off */

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

    normal_status();

    TickType_t last_beat = xTaskGetTickCount();
    TickType_t last_word_tick = 0;
    for (;;) {
        if (s_loop_work_start) {
            int64_t work = esp_timer_get_time() - s_loop_work_start; /* the previous turn, its wait excluded */
            if (work > s_loop_max_us) {
                s_loop_max_us = work;
                s_loop_max_label = s_loop_label;
            }
        }
        word_event_t ev;
        bool got_word = xQueueReceive(s_events, &ev, pdMS_TO_TICKS(250)) == pdTRUE;
        s_loop_work_start = esp_timer_get_time();
        s_loop_label = "idle";
        s_loop_turns++;
        if (got_word) {
            /* One utterance, one card: the engine can fire twice on a word's tail. */
            if (last_word_tick && xTaskGetTickCount() - last_word_tick < pdMS_TO_TICKS(DEDUPE_MS)) {
                ESP_LOGI(TAG, "ignored '%s' %.0f%%: within %d ms of the last word", ev.text, ev.prob * 100, DEDUPE_MS);
            } else {
                s_loop_label = "word"; /* card render, JPEG decode from the card, maybe a sound */
                on_word(&ev);
                last_word_tick = xTaskGetTickCount();
            }
            xQueueReset(s_events); /* anything heard while the chime played was us */
        }
        button_event_t bev = button_poll();
        /* The card can come and go whether we are awake or asleep. */
        bool card_changed = sdcard_poll();
        char cmd = devcmd_take();
        if (bev != BUTTON_NONE || cmd) {
            s_loop_label = bev != BUTTON_NONE ? "button" : "command";
        }
        thermal_step(); /* every turn, in every mode: the guard runs asleep and in maintenance too */
        /* What a press or command lands on, taken before it acts; the record names the outcome. */
        const char *screen_before = screen_name();
        bool asleep_before = s_asleep, maint_before = s_maint;
        uint32_t wake_s = since_wake_s();
        const char *action = NULL;
        if (bev == BUTTON_LONG || cmd == 'm') {
            if (s_maint) {
                leave_maint();
                action = "maint_out";
            } else if (s_thermal.level >= THERMAL_HOT) {
                hot_ack(); /* the radio would only add heat */
                action = "ignored_hot";
            } else {
                enter_maint();
                action = s_maint ? "maint_in" : "maint_failed";
            }
        } else if (bev == BUTTON_SHORT && !s_maint) {
            /* BOOT wakes, and only wakes. It used to sleep a lit screen as well, and on
             * 2026-09-06 the user's own presses put the board to sleep twice in twelve
             * seconds; with "brighten" invisible on a bright screen that read as "black
             * screen, no response to any button". Sleep is the quiet timer's or the
             * serial `s`'s job, and every press now says something on the status line. */
            if (s_thermal.level >= THERMAL_HOT) {
                hot_ack(); /* a board that is cooling stays asleep, and says so for three seconds */
                action = "ignored_hot";
            } else {
                if (s_asleep) {
                    leave_sleep(true);
                    action = "wake";
                } else {
                    action = s_dimmed ? "brighten" : "awake";
                    wake_screen();
                }
                show_ack("awake - hold BOOT for setup");
            }
        } else if (cmd == 's' && !s_maint) {
            if (s_asleep && s_thermal.level >= THERMAL_HOT) {
                action = "ignored_hot";
            } else if (s_asleep) {
                leave_sleep(true);
                action = "wake";
            } else {
                enter_sleep("serial");
                action = "sleep";
            }
        } else if (cmd == 'r' && sdcard_present()) {
            card_arrived(false);
            action = "reload";
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
            action = all_debug ? "debug_all" : "debug_own";
        } else if (cmd == 'p') {
            action = "power";
            pmu_dump_rails("on request");
            pmu_status_t ps;
            if (pmu_read(&ps) == ESP_OK) {
                ESP_LOGI(TAG, "power: usb=%d %u mV, battery=%d %u mV %u%%, vsys %u mV, %s", ps.vbus_in, ps.vbus_mv, ps.batt_present,
                         ps.vbat_mv, ps.batt_pct, ps.vsys_mv, ps.charging ? "charging" : "not charging");
            }
            char chg_txt[120];
            pmu_charger_text(chg_txt, sizeof(chg_txt));
            ESP_LOGI(TAG, "thermal: chip %.1f C, PMU %.1f C, level %s%s; %s", s_thermal.chip_c, s_thermal.pmu_c,
                     thermal_level_name(s_thermal.level), s_thermal.simulated ? " (SIMULATED)" : "", chg_txt);
        } else if (cmd == 'b') {
            s_dimmed = false;
            s_last_activity = xTaskGetTickCount();
            cards_force_bright();
            action = "bright";
        } else if (cmd == 'x') {
            cards_panel_reinit();
            cards_show_idle();
            action = "panel_reinit";
        } else if (cmd == 't') {
            thermal_simulate_step();
            action = "thermal_sim";
        } else if (cmd == 'i') {
            action = "info";
            char now[32];
            ESP_LOGI(TAG, "info: %s heard=%" PRIu32 " words=%u card=%d files=%d log=%ld B maint=%d asleep=%d internal=%u chip=%.1fC pmu=%.1fC thermal=%s",
                     timesync_now_str(now, sizeof(now)), s_heard, (unsigned)s_book.count, sdcard_present(), s_files_ok,
                     sdlog_size(), s_maint, s_asleep, heap_caps_get_free_size(MALLOC_CAP_INTERNAL), s_thermal.chip_c,
                     s_thermal.pmu_c, thermal_level_name(s_thermal.level));
        }
        if (bev != BUTTON_NONE) {
            input_record("boot", bev == BUTTON_LONG ? "long" : "short", button_last_held_ms(), -1, -1, screen_before,
                         asleep_before, maint_before, wake_s, action ? action : "ignored");
        } else if (cmd) {
            char kind[2] = {cmd, '\0'};
            input_record("serial", kind, 0, -1, -1, screen_before, asleep_before, maint_before, wake_s,
                         action ? action : "ignored");
        }
        if (s_maint) {
            xQueueReset(s_events);
            if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
                tap_record("ignored_maint");
            }
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
            s_loop_label = "card";
            if (sdcard_present()) {
                card_arrived(false);
            } else {
                card_left();
            }
        }
        if (s_asleep) {
            xQueueReset(s_events);
            if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
                tap_record("ignored_asleep"); /* the glass still reports with the panel dark */
            }
            pmu_poll(); /* USB in/out and the battery curve are recorded through a sleep as well */
            if (s_hot_ack_until && (int32_t)(xTaskGetTickCount() - s_hot_ack_until) >= 0) {
                s_hot_ack_until = 0;
                bsp_display_brightness_set(0); /* the "cooling down" note has been up long enough */
            }
            continue; /* nothing else runs while asleep; the loop still turns for the button and card */
        }
        if (s_ack_until && (int32_t)(xTaskGetTickCount() - s_ack_until) >= 0) {
            s_ack_until = 0;
            normal_status();
        }
        if (xSemaphoreTake(s_tap, 0) == pdTRUE) {
            s_loop_label = "tap";
            /* Tap: wake the screen if it has dimmed. Nothing else; taps make no sound. */
            const char *what = s_dimmed ? "brighten" : "keep_awake";
            tap_record(what);
            wake_screen();
        }
        if (xTaskGetTickCount() - s_last_activity >= pdMS_TO_TICKS(CONFIG_WORDBOOK_SLEEP_AFTER_S * 60 * 1000)) {
            s_loop_label = "sleep";
            enter_sleep("quiet");
            continue;
        }
        if (pmu_poll()) {
            s_loop_label = "usb"; /* a plug or unplug: rails dumped and a record written */
        }
        maybe_dim();
        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = xTaskGetTickCount();
            char now[32];
            pmu_status_t ps = {0};
            pmu_read(&ps);
            ESP_LOGI(TAG, "alive: %s heard=%" PRIu32 " internal=%u psram=%u card=%d files=%d log=%d words=%u screen=%s usb=%d vbat=%u%s",
                     timesync_now_str(now, sizeof(now)), s_heard, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM), sdcard_present(), s_files_ok, sdlog_active(),
                     (unsigned)s_book.count, s_asleep ? "off" : s_dimmed ? "dim" : "on", ps.vbus_in, ps.vbat_mv,
                     ps.charging ? " charging" : "");
        }
    }
}
