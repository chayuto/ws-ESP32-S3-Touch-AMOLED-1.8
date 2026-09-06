/*
 * 05_dictation - offline speech to text, printed on the glass.
 *
 * Status: this build exists to answer MX-1. See docs/design/05_dictation.md.
 *
 * The question is whether MultiNet can be talked out of its command list. The
 * recogniser loads a small known-good vocabulary so there is a baseline, and logs
 * `raw_string` for EVERY result - detections and, more importantly, timeouts, which
 * is where out-of-vocabulary speech ends up. Say things that are not in the list and
 * read the MX1 lines.
 *
 * Everything else here is the debuggability from 02 carried across so that when this
 * misbehaves it says why: levelled logs, the SD flight recorder, decode records,
 * power telemetry, thermal, and a maintenance mode. The radio is only ever on inside
 * maintenance mode, and audio never leaves the board in any mode.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "button.h"
#include "clog.h"
#include "devcmd.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "maint.h"
#include "pcf85063.h"
#include "pmu.h"
#include "recognizer.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "thermal.h"
#include "timesync.h"

static const char *TAG = "dictation";

#define LOG_PATH      "/sdcard/05_dictation.log"
#define DECODES_PATH  "/sdcard/05_dictation.decodes.jsonl"
#define LOOP_MS       250
#define HEARTBEAT_MS  10000

/*
 * The MX-1 baseline vocabulary. Deliberately small and ordinary: its job is to prove
 * the engine is alive and to give `string` something to snap to, so that a differing
 * `raw_string` is visibly different. Say words OUTSIDE this list - that is the test.
 */
#if CONFIG_DICT_LANG_CN
/* MultiNet Chinese takes pinyin without tone marks. */
static const word_def_t k_probe_words[] = {
    {"da kai"}, {"guan bi"}, {"kai shi"}, {"ting zhi"}, {"ni hao"},
    {"zai jian"}, {"xie xie"}, {"shang"}, {"xia"}, {"zuo"}, {"you"},
};
static const char *k_lang = "cn";
#else
static const word_def_t k_probe_words[] = {
    {"open"}, {"close"}, {"start"}, {"stop"}, {"hello"},
    {"goodbye"}, {"thank you"}, {"up"}, {"down"}, {"left"}, {"right"},
};
static const char *k_lang = "en";
#endif
#define PROBE_WORD_COUNT (sizeof(k_probe_words) / sizeof(k_probe_words[0]))

static QueueHandle_t s_events;
static audio_io_t s_audio;
static bool s_maint;
static uint32_t s_utterances;
static char s_last_text[64] = "";
static float s_last_prob;
static uint32_t s_loop_max_ms, s_loop_turns;

/* Raise this project's own tags to DEBUG; third-party components stay at INFO. */
static void raise_own_log_levels(void)
{
    static const char *tags[] = {"dictation", "recog", "display", "audio_io", "sdcard",
                                 "sdlog", "pmu", "thermal", "timesync", "maint", "button", "devcmd", "clog"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }
}

static void boot_report(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "boot: reset reason %d, internal free %u B, psram free %u B", (int)rr,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    pmu_status_t p = {0};
    if (pmu_read(&p) == ESP_OK) {
        ESP_LOGI(TAG, "boot: battery %u%% at %u mV, vsys %u mV, usb %s, charging %s, pmu %.1f C", p.batt_pct,
                 p.vbat_mv, p.vsys_mv, p.vbus_in ? "in" : "out", p.charging ? "yes" : "no", pmu_die_temp_c());
    }
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    ESP_LOGI(TAG, "boot: card %s, %" PRIu32 " of %" PRIu32 " MB free", sdcard_present() ? "mounted" : "absent",
             free_mb, total_mb);
    ESP_LOGW(TAG, "boot: language %s, MX-1 probe %s - say words OUTSIDE the loaded list and read the MX1 lines",
             k_lang, CONFIG_DICT_PROBE_ON_BOOT ? "ON" : "off");
}

static void fill_state(maint_app_state_t *out)
{
    out->utterances = s_utterances;
    out->last_text = s_last_text;
    out->last_prob = s_last_prob;
    out->loop_max_ms = s_loop_max_ms;
    out->loop_turns = s_loop_turns;
    /* MX-1 tallies live in the recogniser; it prints them itself on demand. */
    out->probe_results = 0;
    out->probe_raw = 0;
    out->probe_differs = 0;
}

static void enter_maint(void)
{
    if (s_maint) {
        return;
    }
    display_message("maintenance", "joining wi-fi...");
    if (maint_start(fill_state) != ESP_OK) {
        display_message("maintenance failed", "could not join wi-fi - press BOOT to go back");
        ESP_LOGE(TAG, "maintenance did not start");
        vTaskDelay(pdMS_TO_TICKS(2500));
        display_message(NULL, NULL);
        return;
    }
    s_maint = true;
    char detail[96];
    snprintf(detail, sizeof(detail), "http://%s/\nhold BOOT to leave", maint_ip());
    display_message("maintenance", detail);
}

static void leave_maint(void)
{
    if (!s_maint) {
        return;
    }
    maint_stop();
    s_maint = false;
    display_message(NULL, NULL);
    display_set_state(DISPLAY_IDLE);
    display_detail("listening - hold BOOT for maintenance");
}

static void handle_devcmd(char c)
{
    switch (c) {
    case 'm': s_maint ? leave_maint() : enter_maint(); break;
    case 'i':
        ESP_LOGI(TAG, "info: %" PRIu32 " utterances, last '%s' %.3f, internal free %u B, loop max %" PRIu32 " ms",
                 s_utterances, s_last_text, s_last_prob, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 s_loop_max_ms);
        break;
    case 'v': recognizer_probe_summary(); break;
    case 'd': esp_log_level_set("*", ESP_LOG_DEBUG); ESP_LOGW(TAG, "all tags at DEBUG"); break;
    case 'p': pmu_dump_rails("serial 'p'"); break;
    case 'b': display_force_bright(); break;
    case 'x': display_panel_reinit(); break;
    case 'c': display_clear(); break;
    case 't': thermal_simulate_step(); break;
    default: break;
    }
}

void app_main(void)
{
    raise_own_log_levels();
    ESP_LOGI(TAG, "05_dictation starting - offline speech to text");

    pmu_init();
    pcf85063_init();
    sdcard_init();
    sdlog_init();
    if (sdcard_present()) {
        sdlog_open(LOG_PATH);
        sdlog_aux_open(0, DECODES_PATH);
    }
    boot_report();

    if (display_start() == NULL) {
        ESP_LOGE(TAG, "display did not come up; continuing headless so the logs still work");
    } else {
        display_init();
        display_set_brightness(CONFIG_DICT_BRIGHTNESS);
        display_set_state(DISPLAY_IDLE);
        display_detail("listening - hold BOOT for maintenance");
    }

    thermal_init();
    button_init();
    devcmd_init();

    if (audio_io_init(&s_audio) != ESP_OK) {
        ESP_LOGE(TAG, "audio did not start; nothing to recognise");
        display_message("no microphone", "audio_io_init failed - see the log");
        return;
    }

    s_events = xQueueCreate(8, sizeof(word_event_t));
    if (recognizer_start(s_audio.mic, k_probe_words, PROBE_WORD_COUNT, s_events) != ESP_OK) {
        ESP_LOGE(TAG, "recogniser did not start");
        display_message("recogniser failed", "see the log");
        return;
    }
    ESP_LOGW(TAG, "listening with %u baseline words; MX-1 is watching raw_string on every result",
             (unsigned)PROBE_WORD_COUNT);

    TickType_t last_beat = xTaskGetTickCount();
    bool was_hearing = false;

    for (;;) {
        TickType_t turn_start = xTaskGetTickCount();

        char c = devcmd_take();
        if (c) {
            handle_devcmd(c);
        }

        button_event_t bev = button_poll();
        if (bev == BUTTON_LONG) {
            s_maint ? leave_maint() : enter_maint();
        } else if (bev == BUTTON_SHORT && !s_maint) {
            /* One press wakes and brightens, and never darkens. Pressing again is what a
             * person does when the screen looks dead, so that is the last thing it should
             * do - 02 learned this the hard way on 2026-09-06. */
            display_set_brightness(CONFIG_DICT_BRIGHTNESS);
            display_detail("listening - hold BOOT for maintenance");
        }

        if (!s_maint) {
            bool hearing = recognizer_hearing_speech();
            if (hearing != was_hearing) {
                display_set_state(hearing ? DISPLAY_HEARING : DISPLAY_IDLE);
                was_hearing = hearing;
            }
            word_event_t ev;
            while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
                s_utterances++;
                snprintf(s_last_text, sizeof(s_last_text), "%s", ev.text ? ev.text : "?");
                s_last_prob = ev.prob;
                char line[96];
                snprintf(line, sizeof(line), "%s  (%.0f%%)", s_last_text, ev.prob * 100.0f);
                display_append(line, ev.prob);
                ESP_LOGI(TAG, "heard '%s' prob=%.3f -> transcript (%" PRIu32 " total)", s_last_text, ev.prob,
                         s_utterances);
            }
        }

        if (s_maint) {
            if (maint_take_reboot()) {
                ESP_LOGW(TAG, "rebooting on request");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            if (maint_idle_s() > CONFIG_DICT_MAINT_IDLE_MIN * 60) {
                ESP_LOGW(TAG, "maintenance idle for %d min; leaving", CONFIG_DICT_MAINT_IDLE_MIN);
                leave_maint();
            }
        }

        thermal_status_t th;
        if (thermal_poll(&th)) {
            ESP_LOGW(TAG, "thermal: %s at %.1f C (chip %.1f, pmu %.1f)", thermal_level_name(th.level), th.board_c,
                     th.chip_c, th.pmu_c);
            if (th.level >= THERMAL_HOT) {
                display_detail("too warm - cooling down");
            }
        }

        sdcard_poll();
        pmu_poll();

        if (xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
            last_beat = xTaskGetTickCount();
            thermal_status_t st;
            thermal_status(&st);
            ESP_LOGI(TAG,
                     "heartbeat: %" PRIu32 " utterances, internal %u B (min %u), psram %u B, card %d, thermal %s, "
                     "loop max %" PRIu32 " ms over %" PRIu32 " turns, maint %d",
                     s_utterances, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), sdcard_present(),
                     thermal_level_name(st.level), s_loop_max_ms, s_loop_turns, (int)s_maint);
            recognizer_probe_summary();
            s_loop_max_ms = 0;
            s_loop_turns = 0;
        }

        uint32_t took = (xTaskGetTickCount() - turn_start) * portTICK_PERIOD_MS;
        if (took > s_loop_max_ms) {
            s_loop_max_ms = took;
        }
        s_loop_turns++;
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
