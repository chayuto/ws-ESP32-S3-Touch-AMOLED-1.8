/*
 * 06_sensorous - measure everything this board can measure, and write it down.
 *
 * Three things run at once:
 *
 *   the sense loop (this task)   buttons, screen, thermal, power, card, and one
 *                                sensor record every cadence period
 *   the scan task                Wi-Fi sweep, then BLE window, then wait. Its own
 *                                task because a sweep blocks for seconds and the
 *                                button must still answer while it does
 *   the IMU task (imu.c)         100 Hz into an accumulator the sense loop drains
 *
 * The radios are never scanned at the same time. One antenna, one 2.4 GHz front
 * end: a Wi-Fi sweep and a BLE window overlapping means both get a fraction of
 * the airtime they asked for and neither result means anything. Sequencing them
 * inside one task is what makes that guarantee cheap.
 *
 * See docs/design/06_sensorous.md.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blescan.h"
#include "bsp/esp-bsp.h"
#include "button.h"
#include "census.h"
#include "devcmd.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu.h"
#include "maint.h"
#include "pcf85063.h"
#include "pmu.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "sound.h"
#include "srec.h"
#include "thermal.h"
#include "timesync.h"
#include "wifi_sta.h"
#include "wifiscan.h"

static const char *TAG = "sensorous";

#define DATA_DIR      BSP_SD_MOUNT_POINT "/sensorous"
#define LOOP_MS       200
#define HEARTBEAT_MS  10000
#define MAINT_IDLE_S  600   /* leave maintenance after ten quiet minutes and go back to work */

/*
 * A crash loop on a device meant to be left alone is worse than a device that
 * does less. Three boots without a healthy run and the radios stay off: the
 * sensors, the card and the screen still work, which is enough to read the log
 * and find out why. Carried from 02 and 05, where it earned its place.
 */
#define BOOT_MAGIC       0x5E4507EDu
#define SAFE_MODE_STREAK 3
#define BOOT_OK_AFTER_MS 120000
static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_streak;
static bool s_safe_mode;

static sensorous_mode_t s_mode =
#if CONFIG_SENSOROUS_MODE_DEFAULT_MOBILE
    MODE_MOBILE;
#else
    MODE_STATIONARY;
#endif
static sensorous_mode_t s_mode_before_maint = MODE_STATIONARY;

static uint32_t s_scan_seq;
static int s_wifi_last = -1, s_ble_last = -1;
static uint32_t s_loop_max_ms, s_loop_turns;
static char s_note[96] = "starting up";
static SemaphoreHandle_t s_scan_gate;   /* given to ask the scan task for a cycle now */
static volatile bool s_scanning_allowed;
static volatile bool s_census_dump_wanted;

/* ------------------------------------------------------------------------- */
/* Boot streak                                                               */
/* ------------------------------------------------------------------------- */

static void boot_streak_begin(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    if (s_boot_magic != BOOT_MAGIC || rr == ESP_RST_POWERON) {
        s_boot_magic = BOOT_MAGIC;
        s_boot_streak = 0;
    }
    s_boot_streak++;
    s_safe_mode = s_boot_streak > SAFE_MODE_STREAK;
    if (s_safe_mode) {
        ESP_LOGE(TAG, "SAFE MODE: %" PRIu32 " boots without a clean run. Radios stay off; sensors and the card "
                      "still run. Power-cycle to clear.", s_boot_streak);
    } else if (s_boot_streak > 1) {
        ESP_LOGW(TAG, "boot streak %" PRIu32 " of %d before safe mode", s_boot_streak, SAFE_MODE_STREAK);
    }
}

static void boot_streak_clear(void)
{
    if (s_boot_streak != 0) {
        ESP_LOGI(TAG, "boot looks healthy; streak cleared");
        s_boot_streak = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void raise_own_log_levels(void)
{
    static const char *tags[] = {"sensorous", "display", "sdcard", "sdlog",  "pmu",      "thermal", "timesync",
                                 "maint",     "button",  "devcmd", "srec",   "census",   "imu",     "wifiscan",
                                 "blescan",   "wifi"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_note, sizeof(s_note), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Card                                                                      */
/* ------------------------------------------------------------------------- */

static void open_files(void)
{
    if (sdlog_open_all(DATA_DIR) == ESP_OK) {
        uint32_t total_mb = 0, free_mb = 0;
        sdcard_space(&total_mb, &free_mb);
        ESP_LOGI(TAG, "recording to %s, %" PRIu32 " of %" PRIu32 " MB free", DATA_DIR, free_mb, total_mb);
    } else {
        ESP_LOGW(TAG, "card is mounted but the files would not open");
    }
}

static void do_eject(const char *why)
{
    /* Order matters: flush and close first, then unmount, then say so. A card
     * pulled between those steps loses whatever the rings had not drained. */
    sdlog_close_all();
    vTaskDelay(pdMS_TO_TICKS(400)); /* let the drain task finish its last pass */
    sdcard_eject();
    srec_event("eject", why);
    note("card ejected - safe to remove");
    display_message("Safe to remove", "The card is unmounted and every file is closed.\nTap MOUNT after putting one back.");
    vTaskDelay(pdMS_TO_TICKS(2500));
    display_clear_message();
}

static void do_mount(const char *why)
{
    display_message("Mounting", "Looking for a card...");
    bool ok = sdcard_remount();
    display_clear_message();
    if (ok) {
        open_files();
        srec_event("mount", why);
        note("card mounted");
    } else {
        note("no card in the slot");
    }
}

/* ------------------------------------------------------------------------- */
/* The scan task                                                             */
/* ------------------------------------------------------------------------- */

static void on_ap(const wifiscan_ap_t *ap, void *ctx)
{
    srec_scan_ap(*(uint32_t *)ctx, ap);
}

static void on_ble(const blescan_dev_t *d, void *ctx)
{
    srec_scan_ble(*(uint32_t *)ctx, d);
}

static int scan_period_s(void)
{
    return s_mode == MODE_MOBILE ? CONFIG_SENSOROUS_SCAN_S_MOBILE : CONFIG_SENSOROUS_SCAN_S_STATIONARY;
}

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Waiting on the gate rather than sleeping flat means 'S' and a mode
         * change take effect now instead of a minute from now. */
        xSemaphoreTake(s_scan_gate, pdMS_TO_TICKS(scan_period_s() * 1000));

        if (s_census_dump_wanted) {
            s_census_dump_wanted = false;
            note("writing the census to the card");
            srec_census_dump();
        }
        if (!s_scanning_allowed) {
            continue;
        }

        uint32_t seq = ++s_scan_seq;
        srec_scan_begin(seq, s_mode);

        int wifi_n = -1, wifi_ms = 0;
#if CONFIG_SENSOROUS_WIFI_SCAN
        note("sweeping Wi-Fi channels");
        wifi_n = wifiscan_run(on_ap, &seq);
        wifi_ms = wifiscan_last_ms();
        s_wifi_last = wifi_n;
#endif

        int ble_n = -1, ble_ms = 0;
#if CONFIG_SENSOROUS_BLE_SCAN
        /* Strictly after the Wi-Fi sweep has returned. Never concurrent. */
        note("listening for Bluetooth");
        int64_t t0 = esp_timer_get_time();
        ble_n = blescan_window(CONFIG_SENSOROUS_BLE_WINDOW_MS, on_ble, &seq);
        ble_ms = (int)((esp_timer_get_time() - t0) / 1000);
        s_ble_last = ble_n;
#endif

        srec_scan_end(seq, wifi_n, wifi_ms, ble_n, ble_ms);
        note("%u Wi-Fi, %u BLE known", (unsigned)census_count(CENSUS_WIFI), (unsigned)census_count(CENSUS_BLE));
    }
}

static void kick_scan(void)
{
    if (s_scan_gate) {
        xSemaphoreGive(s_scan_gate);
    }
}

/* ------------------------------------------------------------------------- */
/* Modes                                                                     */
/* ------------------------------------------------------------------------- */

static void set_mode(sensorous_mode_t m, const char *why)
{
    if (m == s_mode) {
        return;
    }
    sensorous_mode_t was = s_mode;
    s_mode = m;
    s_scanning_allowed = (m != MODE_MAINTENANCE) && !s_safe_mode;
    pmu_set_record_period_s(m == MODE_MOBILE ? CONFIG_SENSOROUS_POWER_LOG_S / 2 : CONFIG_SENSOROUS_POWER_LOG_S);
    ESP_LOGW(TAG, "mode %s -> %s (%s); scanning %s, sensor cadence %d ms, scan every %d s",
             srec_mode_name(was), srec_mode_name(m), why, s_scanning_allowed ? "on" : "off",
             m == MODE_MOBILE ? CONFIG_SENSOROUS_SENSE_MS_MOBILE : CONFIG_SENSOROUS_SENSE_MS_STATIONARY,
             scan_period_s());
    srec_eventf("mode", "%s -> %s (%s)", srec_mode_name(was), srec_mode_name(m), why);
    note("mode: %s", srec_mode_name(m));
    kick_scan();
}

static void maint_state(maint_app_state_t *out);

static void enter_maintenance(const char *why)
{
    s_mode_before_maint = s_mode;
    set_mode(MODE_MAINTENANCE, why);
    display_message("Maintenance", "Joining the network...");
    if (maint_start(maint_state) != ESP_OK) {
        display_message("Maintenance failed", "The network would not have us. Check the credentials in the log.");
        vTaskDelay(pdMS_TO_TICKS(3000));
        display_clear_message();
        set_mode(s_mode_before_maint, "maintenance failed");
        return;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "http://%s/\n\nScanning is paused while this is up.", maint_ip());
    display_message("Maintenance", detail);
    note("serving at %s", maint_ip());
}

static void leave_maintenance(const char *why)
{
    maint_stop();
    display_clear_message();
    set_mode(s_mode_before_maint, why);
}

/* What the HTTP server reports about the app. */
static void maint_state(maint_app_state_t *out)
{
    out->mode = srec_mode_name(s_mode);
    out->scans = s_scan_seq;
    out->wifi_known = census_count(CENSUS_WIFI);
    out->ble_known = census_count(CENSUS_BLE);
    out->wifi_last = s_wifi_last;
    out->ble_last = s_ble_last;
    out->records_sensor = srec_count_sensor();
    out->records_radio = srec_count_radio();
    out->records_event = srec_count_event();
    out->loop_max_ms = s_loop_max_ms;
    out->loop_turns = s_loop_turns;
}

/* pmu.c owns the schedule; this is where its record lands. */
static void power_record(const char *event)
{
    pmu_status_t p = {0};
    if (pmu_read(&p) != ESP_OK) {
        return;
    }
    uint8_t dcdc = 0, ldo0 = 0, ldo1 = 0, irq[3] = {0};
    pmu_rail_bits(&dcdc, &ldo0, &ldo1);
    pmu_irq_status(irq);
    thermal_status_t th;
    thermal_status(&th);
    char now[32], line[420];
    snprintf(line, sizeof(line),
             "{\"t\":\"power\",\"time\":\"%s\",\"up_ms\":%u,\"event\":\"%s\",\"mode\":\"%s\","
             "\"vbat_mv\":%u,\"batt_pct\":%u,\"vbus\":%s,\"vbus_mv\":%u,\"vsys_mv\":%u,"
             "\"charging\":%s,\"chg_state\":%u,\"batt_present\":%s,"
             "\"rails\":{\"dcdc\":%u,\"ldo0\":%u,\"ldo1\":%u},\"irq\":[%u,%u,%u],"
             "\"chip_c\":%.1f,\"pmu_c\":%.1f}",
             srec_iso_time(now, sizeof(now)), (unsigned)(esp_timer_get_time() / 1000), event,
             srec_mode_name(s_mode), (unsigned)p.vbat_mv, (unsigned)p.batt_pct, p.vbus_in ? "true" : "false",
             (unsigned)p.vbus_mv, (unsigned)p.vsys_mv, p.charging ? "true" : "false", (unsigned)p.chg_state,
             p.batt_present ? "true" : "false", dcdc, ldo0, ldo1, irq[0], irq[1], irq[2], (double)th.chip_c,
             (double)th.pmu_c);
    sdlog_aux_write(SDLOG_CH_POWER, line);
}

/* ------------------------------------------------------------------------- */
/* Serial commands                                                           */
/* ------------------------------------------------------------------------- */

static void status_line(void)
{
    thermal_status_t th;
    thermal_status(&th);
    pmu_status_t p = {0};
    pmu_read(&p);
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    imu_stats_t im;
    imu_peek(&im);
    ESP_LOGW(TAG,
             "status: mode %s, scans %" PRIu32 ", wifi %u known (%d last), ble %u known (%d last, %u adverts), "
             "records %u/%u/%u, card %s %" PRIu32 " MB free, batt %u%%, board %.1f C, internal %u B",
             srec_mode_name(s_mode), s_scan_seq, (unsigned)census_count(CENSUS_WIFI), s_wifi_last,
             (unsigned)census_count(CENSUS_BLE), s_ble_last, (unsigned)blescan_adverts(),
             (unsigned)srec_count_sensor(), (unsigned)srec_count_radio(), (unsigned)srec_count_event(),
             sdcard_ejected() ? "ejected" : (sdcard_present() ? "in" : "absent"), free_mb, p.batt_pct,
             (double)th.board_c, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (im.valid) {
        ESP_LOGW(TAG, "status: imu %u samples, tilt %+.1f/%+.1f, motion rms %.3f, peak %.2f, %.1f C", im.samples,
                 (double)im.pitch_deg, (double)im.roll_deg, (double)im.a_dyn_rms, (double)im.a_dyn_peak,
                 (double)im.temp_c);
    }
    sound_level_t snd;
    sound_last(&snd);
    uint32_t bursts = 0, failures = 0;
    sound_counts(&bursts, &failures);
    ESP_LOGW(TAG, "status: sound %.1f dBFS rms, %.1f peak, %u clipped, %u bursts (%u failed), mic closed now",
             (double)snd.rms_dbfs, (double)snd.peak_dbfs, (unsigned)snd.clipped, (unsigned)bursts,
             (unsigned)failures);
}

static void handle_command(char c)
{
    switch (c) {
    case 'm':
        if (maint_active()) {
            leave_maintenance("serial");
        } else {
            enter_maintenance("serial");
        }
        break;
    case 'M':
        if (s_mode == MODE_MAINTENANCE) {
            ESP_LOGW(TAG, "leave maintenance first");
        } else {
            set_mode(s_mode == MODE_MOBILE ? MODE_STATIONARY : MODE_MOBILE, "serial");
        }
        break;
    case 'S': ESP_LOGW(TAG, "scan now"); kick_scan(); break;
    case 'e': do_eject("serial"); break;
    case 'o': do_mount("serial"); break;
    case 'c': s_census_dump_wanted = true; kick_scan(); break;
    case 'z': census_reset(); srec_event("census_reset", "serial"); break;
    case 'i': status_line(); break;
    case 'd': esp_log_level_set("*", ESP_LOG_DEBUG); ESP_LOGW(TAG, "all tags at DEBUG"); break;
    case 'p': pmu_dump_rails("serial"); break;
    case 'b': display_force_bright(); break;
    case 'x': display_panel_reinit(); break;
    case 't': thermal_simulate_step(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */

static void fill_display(display_status_t *d)
{
    thermal_status_t th;
    thermal_status(&th);
    pmu_status_t p = {0};
    pmu_read(&p);
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    imu_stats_t im;
    imu_peek(&im);

    static char clock_buf[16];
    time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &tm);
    } else {
        strlcpy(clock_buf, "unset", sizeof(clock_buf));
    }

    d->mode = srec_mode_name(s_mode);
    d->clock = clock_buf;
    d->note = s_note;
    d->card = sdcard_present();
    d->card_ejected = sdcard_ejected();
    d->card_free_mb = free_mb;
    d->records = srec_count_sensor() + srec_count_radio() + srec_count_event();
    d->batt_pct = p.batt_pct;
    d->vbus = p.vbus_in;
    d->charging = p.charging;
    d->board_c = th.board_c;
    d->wifi_known = census_count(CENSUS_WIFI);
    d->ble_known = census_count(CENSUS_BLE);
    d->wifi_last = s_wifi_last;
    d->ble_last = s_ble_last;
    d->scans = s_scan_seq;
    d->imu_ok = im.valid;
    d->imu_pitch = im.pitch_deg;
    d->imu_roll = im.roll_deg;
    d->imu_dyn = im.a_dyn_rms;
    sound_level_t snd;
    sound_last(&snd);
    d->sound_ok = snd.valid;
    d->sound_dbfs = snd.rms_dbfs;
    d->ip = maint_active() ? maint_ip() : NULL;
}

void app_main(void)
{
    raise_own_log_levels();
    boot_streak_begin();
    ESP_LOGI(TAG, "06_sensorous starting - measure everything, write it to the card");

    /* The BSP logs a pull-up warning on every I2C init; the board has hardware
     * pull-ups and every device enumerates. Quiet across bring-up, restored after. */
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);

    sdlog_init();
    pmu_init();
    pcf85063_init();
    imu_init();
    sdcard_init();
    if (sdcard_present()) {
        open_files();
    }

    /*
     * The screen before the radio, for two reasons. LVGL's draw buffer must land
     * in internal, DMA-capable RAM (see display_start), so it is claimed while
     * internal RAM is at its emptiest. And the NTP join below blocks for up to
     * twenty seconds - long enough that a dark screen reads as a dead board,
     * which is the fault this repo has now diagnosed twice.
     */
    if (display_start() == NULL) {
        ESP_LOGE(TAG, "display did not come up; continuing headless so the logs still work");
    } else {
        display_init();
        display_set_brightness(CONFIG_SENSOROUS_BRIGHTNESS);
        display_message("Sensorous", "Setting the clock...");
    }
    esp_log_level_set("i2c.master", ESP_LOG_WARN);

    /*
     * The clock before anything that writes a record: a timestamp of 1970 in the
     * first hour of a survey is a hole in the data that cannot be filled in
     * afterwards. NTP if the network is configured, then the RTC, then the build
     * time. It leaves the Wi-Fi driver up, which is what the scanner wants next.
     */
    timesync_at_boot();
    display_clear_message();

    thermal_init();
    button_init();
    devcmd_init();
#if CONFIG_SENSOROUS_SOUND
    if (sound_init() != ESP_OK) {
        ESP_LOGW(TAG, "no ambient level; every other sensor still records");
    }
#endif
    pmu_set_record_cb(power_record);

    if (!census_init()) {
        ESP_LOGE(TAG, "no census tables; addresses will not be remembered");
    }

    /*
     * Radios last, and not at all in safe mode. Both stacks stay resident for the
     * whole run: bringing Wi-Fi up and down per scan costs seconds and, as
     * CLAUDE.md records, does not give the internal RAM back anyway.
     */
    if (!s_safe_mode) {
        if (wifi_radio_up() == ESP_OK) {
            wifiscan_init();
        }
#if CONFIG_SENSOROUS_BLE_SCAN
        if (blescan_init() != ESP_OK) {
            ESP_LOGE(TAG, "BLE observer did not start; Wi-Fi scanning continues alone");
        }
#endif
    }

    ESP_LOGI(TAG, "after init: internal free %u B (min %u), psram free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    srec_boot(s_mode);
    if (s_safe_mode) {
        srec_event("safe_mode", "three boots without a healthy run; radios are off");
        note("SAFE MODE - radios off");
    }

    s_scan_gate = xSemaphoreCreateBinary();
    s_scanning_allowed = !s_safe_mode;
    xTaskCreatePinnedToCore(scan_task, "scan", 4096, NULL, 4, NULL, 1);
    kick_scan(); /* do not make the first cycle wait a whole period */

    int64_t last_sense = 0, last_beat = 0;
    for (;;) {
        int64_t turn_start = esp_timer_get_time();
        s_loop_turns++;

        char c = devcmd_take();
        if (c) {
            handle_command(c);
        }

        switch (button_poll()) {
        case BUTTON_SHORT:
            /* One press, one meaning. See CLAUDE.md on why there is no gesture here. */
            if (maint_active()) {
                leave_maintenance("BOOT short press");
            } else {
                set_mode(s_mode == MODE_MOBILE ? MODE_STATIONARY : MODE_MOBILE, "BOOT short press");
            }
            break;
        case BUTTON_LONG:
            if (maint_active()) {
                leave_maintenance("BOOT long press");
            } else {
                enter_maintenance("BOOT long press");
            }
            break;
        default:
            break;
        }

        if (display_take_card_tap()) {
            if (sdcard_present()) {
                do_eject("screen button");
            } else {
                do_mount("screen button");
            }
        }

        switch (maint_take_request()) {
        case MAINT_REQ_EJECT: do_eject("http"); break;
        case MAINT_REQ_MOUNT: do_mount("http"); break;
        case MAINT_REQ_CENSUS: s_census_dump_wanted = true; kick_scan(); break;
        case MAINT_REQ_REBOOT:
            ESP_LOGW(TAG, "reboot requested; closing files first");
            srec_event("reboot", "http");
            sdlog_close_all();
            vTaskDelay(pdMS_TO_TICKS(600));
            esp_restart();
            break;
        default: break;
        }

        /* A card that appeared or vanished on its own - there is no detect pin,
         * so this is a poll, and it is the only warning of a card pulled live. */
        if (sdcard_poll()) {
            if (sdcard_present()) {
                open_files();
                srec_event("card", "inserted");
                note("card inserted");
            } else {
                sdlog_close_all();
                srec_event("card", "removed without an eject");
                ESP_LOGW(TAG, "card removed without an eject; whatever had not drained is gone");
                note("card removed - nothing is being saved");
            }
        }
        sdlog_maintain();

        thermal_status_t th;
        if (thermal_poll(&th)) {
            srec_eventf("thermal", "level %s at %.1f C (chip %.1f, pmu %.1f)", thermal_level_name(th.level),
                        (double)th.board_c, (double)th.chip_c, (double)th.pmu_c);
            switch (th.level) {
            case THERMAL_WARM:
                display_set_brightness(CONFIG_SENSOROUS_BRIGHTNESS / 3);
                note("warm (%.0f C) - screen dimmed", (double)th.board_c);
                break;
            case THERMAL_HOT:
                /* The radios are the biggest thing this app can switch off. */
                s_scanning_allowed = false;
                pmu_set_charging(false);
                display_set_brightness(10);
                ESP_LOGE(TAG, "hot at %.1f C: scanning stopped and charging off", (double)th.board_c);
                note("HOT (%.0f C) - scanning stopped", (double)th.board_c);
                break;
            case THERMAL_TRIP:
                ESP_LOGE(TAG, "thermal trip at %.1f C: closing the files and powering off", (double)th.board_c);
                thermal_mark_trip(&th);
                sdlog_close_all();
                vTaskDelay(pdMS_TO_TICKS(800));
                pmu_power_off();
                break;
            case THERMAL_OK:
            default:
                s_scanning_allowed = (s_mode != MODE_MAINTENANCE) && !s_safe_mode;
                pmu_set_charging(true);
                display_set_brightness(CONFIG_SENSOROUS_BRIGHTNESS);
                note("cooled to %.0f C - scanning resumed", (double)th.board_c);
                break;
            }
        }
        pmu_poll();

        if (maint_active() && maint_idle_s() > MAINT_IDLE_S) {
            ESP_LOGW(TAG, "maintenance idle for %d s; going back to work", maint_idle_s());
            leave_maintenance("idle timeout");
        }

        /* --- the sensor record, on its cadence ------------------------- */
        int sense_ms = (s_mode == MODE_MOBILE) ? CONFIG_SENSOROUS_SENSE_MS_MOBILE
                                               : CONFIG_SENSOROUS_SENSE_MS_STATIONARY;
        if (turn_start - last_sense > (int64_t)sense_ms * 1000) {
            last_sense = turn_start;
            imu_stats_t im;
            imu_take(&im);
            /* A short burst, here on the sense loop rather than on a task of its
             * own: the mic is open for a fifth of a second and shut the rest of
             * the time, which is the property worth having. */
            sound_level_t snd = {0};
#if CONFIG_SENSOROUS_SOUND
            sound_measure(&snd);
#endif
            srec_sensor(s_mode, &im, &snd);
        }

        display_status_t d = {0};
        fill_display(&d);
        display_update(&d);

        if (turn_start - last_beat > (int64_t)HEARTBEAT_MS * 1000) {
            last_beat = turn_start;
            thermal_status(&th);
            uint32_t total_mb = 0, free_mb = 0;
            sdcard_space(&total_mb, &free_mb);
            ESP_LOGI(TAG,
                     "heartbeat: mode %s, %" PRIu32 " scans, wifi %u/%u ble %u/%u, records %u+%u+%u, "
                     "card %s %" PRIu32 " MB, internal %u B (min %u), psram %u B, board %.1f C, "
                     "loop max %" PRIu32 " ms over %" PRIu32 " turns, drops %u",
                     srec_mode_name(s_mode), s_scan_seq, (unsigned)census_count(CENSUS_WIFI),
                     (unsigned)census_capacity(CENSUS_WIFI), (unsigned)census_count(CENSUS_BLE),
                     (unsigned)census_capacity(CENSUS_BLE), (unsigned)srec_count_sensor(),
                     (unsigned)srec_count_radio(), (unsigned)srec_count_event(),
                     sdcard_ejected() ? "ejected" : (sdcard_present() ? "in" : "absent"), free_mb,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (double)th.board_c, s_loop_max_ms,
                     s_loop_turns, (unsigned)sdlog_dropped());
            s_loop_max_ms = 0;
            s_loop_turns = 0;
        }

        if (s_boot_streak != 0 && esp_timer_get_time() / 1000 > BOOT_OK_AFTER_MS) {
            boot_streak_clear();
        }

        uint32_t took = (uint32_t)((esp_timer_get_time() - turn_start) / 1000);
        if (took > s_loop_max_ms) {
            s_loop_max_ms = took;
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
