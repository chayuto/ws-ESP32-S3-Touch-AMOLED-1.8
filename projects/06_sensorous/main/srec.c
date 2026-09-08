#include "srec.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "census.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf85063.h"
#include "pmu.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "thermal.h"
#include "timesync.h"

static const char *TAG = "srec";

static uint32_t s_n_sensor, s_n_radio, s_n_event;

const char *srec_mode_name(sensorous_mode_t m)
{
    switch (m) {
        case MODE_STATIONARY: return "stationary";
        case MODE_MOBILE: return "mobile";
        case MODE_MAINTENANCE: return "maintenance";
        default: return "?";
    }
}

const char *srec_iso_time(char *buf, size_t n)
{
    time_t now = time(NULL);
    /* Before a sync the epoch is near zero; saying "unset" is more honest than
     * writing 1970 and letting a reader believe it. */
    if (now < 1600000000) {
        snprintf(buf, n, "unset");
        return buf;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/*
 * SSIDs and BLE local names are arbitrary bytes off the air, and a stray quote
 * would break every downstream parser on the line it landed in. Escape once,
 * here, and drop anything outside printable ASCII - the alternative is a file
 * that is valid until the day someone names their network `"; DROP`.
 */
static void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in != NULL && in[i] != '\0' && o + 2 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c > 0x7e) {
            out[o++] = '.';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* NAN in a JSON number is not JSON. Absent sensors write null. */
static const char *fnum(float v, char *buf, size_t n, int decimals)
{
    if (isnan(v) || isinf(v)) {
        snprintf(buf, n, "null");
    } else {
        snprintf(buf, n, "%.*f", decimals, v);
    }
    return buf;
}

static uint32_t up_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void srec_boot(sensorous_mode_t mode)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    const esp_app_desc_t *app = esp_app_get_description();

    uint8_t sta[6] = {0}, bt[6] = {0};
    esp_read_mac(sta, ESP_MAC_WIFI_STA);
    esp_read_mac(bt, ESP_MAC_BT);
    char sta_s[18], bt_s[18];
    census_mac_str(sta, sta_s, sizeof(sta_s));
    census_mac_str(bt, bt_s, sizeof(bt_s));

    timesync_info_t ts;
    timesync_info(&ts);

    uint8_t who = 0, rev = 0;
    imu_ident(&who, &rev);

    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);

    char now[32];
    char line[640];
    snprintf(line, sizeof(line),
             "{\"t\":\"boot\",\"time\":\"%s\",\"up_ms\":%u,\"reset\":%d,\"mode\":\"%s\","
             "\"app\":{\"name\":\"%s\",\"ver\":\"%s\",\"idf\":\"%s\",\"built\":\"%s %s\"},"
             "\"chip\":{\"cores\":%d,\"rev\":%d,\"freq_mhz\":%d,\"flash_mb\":%u,\"psram_mb\":%u},"
             "\"mac\":{\"sta\":\"%s\",\"bt\":\"%s\"},"
             "\"clock\":{\"source\":\"%s\",\"rtc_valid\":%s,\"drift_s\":%d,\"drift_known\":%s,\"ntp_ms\":%d},"
             "\"imu\":{\"present\":%s,\"who_am_i\":%u},"
             "\"card\":{\"present\":%s,\"total_mb\":%u,\"free_mb\":%u,\"dir\":\"%s\"}}",
             srec_iso_time(now, sizeof(now)), (unsigned)up_ms(), (int)esp_reset_reason(), srec_mode_name(mode),
             app->project_name, app->version, app->idf_ver, app->date, app->time, chip.cores, chip.revision,
             (int)(esp_clk_cpu_freq() / 1000000), (unsigned)(flash_size >> 20),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20), sta_s, bt_s, ts.clock,
             ts.rtc_valid ? "true" : "false", ts.rtc_drift_s, ts.drift_known ? "true" : "false", ts.ntp_ms,
             imu_present() ? "true" : "false", (unsigned)who, sdcard_present() ? "true" : "false",
             (unsigned)total_mb, (unsigned)free_mb, sdlog_dir());
    sdlog_aux_write(SDLOG_CH_EVENTS, line);
    s_n_event++;
    ESP_LOGI(TAG, "boot record written");
}

void srec_sensor(sensorous_mode_t mode, const imu_stats_t *imu, const sound_level_t *snd)
{
    thermal_status_t th;
    thermal_status(&th);
    pmu_status_t p = {0};
    pmu_read(&p);
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);

    char now[32];
    char b[8][16];
    char line[1024];

    int n = snprintf(line, sizeof(line),
                     "{\"t\":\"sensor\",\"time\":\"%s\",\"up_ms\":%u,\"mode\":\"%s\",",
                     srec_iso_time(now, sizeof(now)), (unsigned)up_ms(), srec_mode_name(mode));

    if (imu != NULL && imu->valid) {
        n += snprintf(line + n, sizeof(line) - n,
                      "\"imu\":{\"n\":%u,\"err\":%u,\"ax\":%s,\"ay\":%s,\"az\":%s,\"amag\":%s,"
                      "\"dyn_rms\":%s,\"dyn_peak\":%s,",
                      (unsigned)imu->samples, (unsigned)imu->read_errors, fnum(imu->ax, b[0], 16, 3),
                      fnum(imu->ay, b[1], 16, 3), fnum(imu->az, b[2], 16, 3), fnum(imu->a_mag_mean, b[3], 16, 3),
                      fnum(imu->a_dyn_rms, b[4], 16, 4), fnum(imu->a_dyn_peak, b[5], 16, 3));
        n += snprintf(line + n, sizeof(line) - n,
                      "\"gx\":%s,\"gy\":%s,\"gz\":%s,\"g_peak\":%s,\"pitch\":%s,\"roll\":%s,\"temp_c\":%s},",
                      fnum(imu->gx, b[0], 16, 2), fnum(imu->gy, b[1], 16, 2), fnum(imu->gz, b[2], 16, 2),
                      fnum(imu->g_mag_peak, b[3], 16, 2), fnum(imu->pitch_deg, b[4], 16, 1),
                      fnum(imu->roll_deg, b[5], 16, 1), fnum(imu->temp_c, b[6], 16, 1));
    } else {
        n += snprintf(line + n, sizeof(line) - n, "\"imu\":null,");
    }

    /* dBFS, DC removed, uncalibrated. `clipped` says whether to believe it. */
    if (snd != NULL && snd->valid) {
        n += snprintf(line + n, sizeof(line) - n,
                      "\"sound\":{\"rms_dbfs\":%s,\"peak_dbfs\":%s,\"n\":%u,\"clipped\":%u,\"ms\":%u},",
                      fnum(snd->rms_dbfs, b[0], 16, 1), fnum(snd->peak_dbfs, b[1], 16, 1),
                      (unsigned)snd->samples, (unsigned)snd->clipped, (unsigned)snd->ms);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "\"sound\":null,");
    }

    n += snprintf(line + n, sizeof(line) - n,
                  "\"temp\":{\"chip_c\":%s,\"pmu_c\":%s,\"board_c\":%s,\"level\":\"%s\"},",
                  fnum(th.chip_c, b[0], 16, 1), fnum(th.pmu_c, b[1], 16, 1), fnum(th.board_c, b[2], 16, 1),
                  thermal_level_name(th.level));

    n += snprintf(line + n, sizeof(line) - n,
                  "\"power\":{\"vbat_mv\":%u,\"batt_pct\":%u,\"vbus\":%s,\"vbus_mv\":%u,\"vsys_mv\":%u,"
                  "\"charging\":%s,\"chg_state\":%u},",
                  (unsigned)p.vbat_mv, (unsigned)p.batt_pct, p.vbus_in ? "true" : "false", (unsigned)p.vbus_mv,
                  (unsigned)p.vsys_mv, p.charging ? "true" : "false", (unsigned)p.chg_state);

    n += snprintf(line + n, sizeof(line) - n,
                  "\"heap\":{\"int_free\":%u,\"int_min\":%u,\"psram_free\":%u},",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint32_t rotations = 0, deletions = 0;
    sdlog_maintenance_counts(&rotations, &deletions);
    n += snprintf(line + n, sizeof(line) - n,
                  "\"card\":{\"present\":%s,\"free_mb\":%u,\"log_kb\":%ld,\"drops\":%u,\"io_err\":%u,"
                  "\"rotations\":%u,\"deletions\":%u},",
                  sdcard_present() ? "true" : "false", (unsigned)free_mb, sdlog_size() / 1024,
                  (unsigned)sdlog_dropped(), (unsigned)sdcard_io_errors(), (unsigned)rotations,
                  (unsigned)deletions);

    snprintf(line + n, sizeof(line) - n, "\"radio\":{\"wifi_known\":%u,\"ble_known\":%u}}",
             (unsigned)census_count(CENSUS_WIFI), (unsigned)census_count(CENSUS_BLE));

    sdlog_aux_write(SDLOG_CH_SENSORS, line);
    s_n_sensor++;
}

void srec_scan_begin(uint32_t seq, sensorous_mode_t mode)
{
    char now[32];
    char line[200];
    snprintf(line, sizeof(line), "{\"t\":\"scan\",\"seq\":%u,\"time\":\"%s\",\"up_ms\":%u,\"mode\":\"%s\"}",
             (unsigned)seq, srec_iso_time(now, sizeof(now)), (unsigned)up_ms(), srec_mode_name(mode));
    sdlog_aux_write(SDLOG_CH_RADIO, line);
    s_n_radio++;
}

void srec_scan_ap(uint32_t seq, const wifiscan_ap_t *ap)
{
    char mac[18], ssid[80];
    char line[260];
    census_mac_str(ap->bssid, mac, sizeof(mac));
    json_escape(ap->ssid, ssid, sizeof(ssid));
    /* macAddress / signalStrength / channel / age are Google Geolocation API
     * field names, on purpose. age is 0 because the sighting is from this scan. */
    snprintf(line, sizeof(line),
             "{\"t\":\"ap\",\"seq\":%u,\"macAddress\":\"%s\",\"signalStrength\":%d,\"channel\":%u,\"age\":0,"
             "\"ssid\":\"%s\",\"auth\":\"%s\",\"new\":%s}",
             (unsigned)seq, mac, ap->rssi, (unsigned)ap->channel, ssid, wifiscan_auth_name(ap->authmode),
             ap->is_new ? "true" : "false");
    sdlog_aux_write(SDLOG_CH_RADIO, line);
    s_n_radio++;
}

void srec_scan_ble(uint32_t seq, const blescan_dev_t *d)
{
    char mac[18], name[80];
    char line[300];
    census_mac_str(d->mac, mac, sizeof(mac));
    json_escape(d->name, name, sizeof(name));
    snprintf(line, sizeof(line),
             "{\"t\":\"ble\",\"seq\":%u,\"macAddress\":\"%s\",\"addrType\":\"%s\",\"signalStrength\":%d,"
             "\"rssi_last\":%d,\"adverts\":%u,\"name\":\"%s\",\"company\":%d,\"tx\":%d,\"conn\":%s,\"new\":%s}",
             (unsigned)seq, mac, census_addr_type_name(d->addr_type), d->rssi_best, d->rssi_last,
             (unsigned)d->adverts, name, (int)d->company, (int)d->tx_power, d->connectable ? "true" : "false",
             d->is_new ? "true" : "false");
    sdlog_aux_write(SDLOG_CH_RADIO, line);
    s_n_radio++;
}

void srec_scan_end(uint32_t seq, int wifi_n, int wifi_ms, int ble_n, int ble_ms)
{
    char line[280];
    snprintf(line, sizeof(line),
             "{\"t\":\"scan_end\",\"seq\":%u,\"wifi_n\":%d,\"wifi_ms\":%d,\"ble_n\":%d,\"ble_ms\":%d,"
             "\"wifi_known\":%u,\"ble_known\":%u,\"ble_adverts\":%u,\"wifi_full\":%s,\"ble_full\":%s}",
             (unsigned)seq, wifi_n, wifi_ms, ble_n, ble_ms, (unsigned)census_count(CENSUS_WIFI),
             (unsigned)census_count(CENSUS_BLE), (unsigned)blescan_adverts(),
             census_overflow(CENSUS_WIFI) ? "true" : "false", census_overflow(CENSUS_BLE) ? "true" : "false");
    sdlog_aux_write(SDLOG_CH_RADIO, line);
    s_n_radio++;
}

void srec_census_dump(void)
{
    char now[32], mac[18], text[80];
    char line[340];
    char stamp[40];
    srec_iso_time(now, sizeof(now));
    snprintf(stamp, sizeof(stamp), "%s", now);

    for (int k = 0; k < 2; k++) {
        size_t n = census_count((census_kind_t)k);
        ESP_LOGI(TAG, "dumping %u %s rows", (unsigned)n, k == CENSUS_WIFI ? "wifi" : "ble");
        for (size_t i = 0; i < n; i++) {
            census_row_t r;
            if (!census_row((census_kind_t)k, i, &r)) {
                break;
            }
            census_mac_str(r.mac, mac, sizeof(mac));
            if (k == CENSUS_WIFI) {
                json_escape(r.ssid, text, sizeof(text));
                snprintf(line, sizeof(line),
                         "{\"t\":\"census_wifi\",\"at\":\"%s\",\"macAddress\":\"%s\",\"ssid\":\"%s\","
                         "\"channel\":%u,\"auth\":\"%s\",\"hidden\":%s,\"rssi_best\":%d,\"rssi_last\":%d,"
                         "\"sightings\":%u,\"first_s\":%lld,\"last_s\":%lld,\"first_up_ms\":%u,\"last_up_ms\":%u}",
                         stamp, mac, text, (unsigned)r.channel, wifiscan_auth_name(r.authmode),
                         r.hidden ? "true" : "false", r.rssi_best, r.rssi_last, (unsigned)r.sightings,
                         (long long)r.first_s, (long long)r.last_s, (unsigned)r.first_up_ms,
                         (unsigned)r.last_up_ms);
            } else {
                json_escape(r.name, text, sizeof(text));
                snprintf(line, sizeof(line),
                         "{\"t\":\"census_ble\",\"at\":\"%s\",\"macAddress\":\"%s\",\"addrType\":\"%s\","
                         "\"name\":\"%s\",\"company\":%d,\"conn\":%s,\"rssi_best\":%d,\"rssi_last\":%d,"
                         "\"sightings\":%u,\"first_s\":%lld,\"last_s\":%lld,\"first_up_ms\":%u,\"last_up_ms\":%u}",
                         stamp, mac, census_addr_type_name(r.addr_type), text, (int)r.company,
                         r.connectable ? "true" : "false", r.rssi_best, r.rssi_last, (unsigned)r.sightings,
                         (long long)r.first_s, (long long)r.last_s, (unsigned)r.first_up_ms,
                         (unsigned)r.last_up_ms);
            }
            sdlog_aux_write(SDLOG_CH_RADIO, line);
            s_n_radio++;
            /* A thousand rows would overrun a 32 KB ring in a few milliseconds.
             * Yield often enough that the drain task keeps up with us. */
            if ((i & 0x1f) == 0x1f) {
                vTaskDelay(pdMS_TO_TICKS(60));
            }
        }
    }
    srec_event("census_dump", "both tables written to radio.jsonl");
}

void srec_event(const char *what, const char *detail)
{
    char now[32], esc[200];
    char line[380];
    json_escape(detail, esc, sizeof(esc));
    snprintf(line, sizeof(line), "{\"t\":\"event\",\"time\":\"%s\",\"up_ms\":%u,\"what\":\"%s\",\"detail\":\"%s\"}",
             srec_iso_time(now, sizeof(now)), (unsigned)up_ms(), what, esc);
    sdlog_aux_write(SDLOG_CH_EVENTS, line);
    s_n_event++;
}

void srec_eventf(const char *what, const char *fmt, ...)
{
    char detail[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    srec_event(what, detail);
}

uint32_t srec_count_sensor(void) { return s_n_sensor; }
uint32_t srec_count_radio(void) { return s_n_radio; }
uint32_t srec_count_event(void) { return s_n_event; }
