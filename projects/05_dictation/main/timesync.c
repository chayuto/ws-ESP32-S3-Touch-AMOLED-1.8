#include "timesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pcf85063.h"
#include "sdkconfig.h"

#if CONFIG_DICT_NTP
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "wifi_sta.h"
#endif

static const char *TAG = "time";

#define VALID_AFTER_YEAR 2025

static timesync_info_t s_info = {.clock = "none"};

static bool clock_is_set(void)
{
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    return t.tm_year + 1900 >= VALID_AFTER_YEAR;
}

static void set_clock(const struct tm *t, bool is_utc, const char *source)
{
    struct tm copy = *t;
    setenv("TZ", is_utc ? "UTC0" : CONFIG_DICT_TZ, 1);
    tzset();
    copy.tm_isdst = -1;
    time_t secs = mktime(&copy);
    struct timeval tv = {.tv_sec = secs};
    settimeofday(&tv, NULL);
    setenv("TZ", CONFIG_DICT_TZ, 1);
    tzset();
    char buf[32];
    ESP_LOGI(TAG, "clock set from %s: %s", source, timesync_now_str(buf, sizeof(buf)));
}

static time_t utc_to_time(const struct tm *t)
{
    struct tm copy = *t;
    setenv("TZ", "UTC0", 1);
    tzset();
    copy.tm_isdst = -1;
    time_t secs = mktime(&copy);
    setenv("TZ", CONFIG_DICT_TZ, 1);
    tzset();
    return secs;
}

static bool from_rtc(void)
{
    struct tm t;
    if (pcf85063_get(&t) != ESP_OK || t.tm_year + 1900 < VALID_AFTER_YEAR) {
        return false;
    }
    set_clock(&t, true, "RTC");
    s_info.clock = "rtc";
    return true;
}

static void from_build_time(void)
{
    /* "Sep  5 2026" / "14:32:10" in the build machine's local time, which is this
     * time zone. A floor, not a fix: every boot without RTC or NTP restarts here. */
    const esp_app_desc_t *app = esp_app_get_description();
    struct tm t = {0};
    char mon[4] = {0};
    static const char *names = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (sscanf(app->date, "%3s %d %d", mon, &t.tm_mday, &t.tm_year) != 3 ||
        sscanf(app->time, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec) != 3) {
        return;
    }
    const char *p = strstr(names, mon);
    t.tm_mon = p ? (int)(p - names) / 3 : 0;
    t.tm_year -= 1900;
    set_clock(&t, false, "firmware build time (approximate)");
    s_info.clock = "build";
}

static void write_rtc_from_clock(void)
{
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    if (pcf85063_set(&t) == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated");
    }
}

#if CONFIG_DICT_NTP
static bool from_ntp(void)
{
    /* What the RTC says now, so the sync can measure how far it had drifted. */
    struct tm rtc_tm;
    bool rtc_ok = pcf85063_get(&rtc_tm) == ESP_OK && rtc_tm.tm_year + 1900 >= VALID_AFTER_YEAR;
    time_t rtc_secs = rtc_ok ? utc_to_time(&rtc_tm) : 0;
    int64_t t_start = esp_timer_get_time();
    if (!wifi_sta_join(CONFIG_DICT_NTP_TIMEOUT_S)) {
        s_info.ntp_ms = (int)((esp_timer_get_time() - t_start) / 1000);
        return false;
    }
    bool ok = false;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DICT_NTP_SERVER);
    esp_netif_sntp_init(&sc);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        setenv("TZ", CONFIG_DICT_TZ, 1);
        tzset();
        char buf[32];
        ESP_LOGI(TAG, "clock set from NTP (%s): %s", CONFIG_DICT_NTP_SERVER, timesync_now_str(buf, sizeof(buf)));
        s_info.clock = "ntp";
        if (rtc_ok) {
            /* The RTC as it would read now, against the freshly synced clock. */
            time_t rtc_now = rtc_secs + (time_t)((esp_timer_get_time() - t_start) / 1000000);
            s_info.rtc_drift_s = (int)(rtc_now - time(NULL));
            s_info.drift_known = true;
            ESP_LOGI(TAG, "RTC was %d s %s of NTP", abs(s_info.rtc_drift_s), s_info.rtc_drift_s >= 0 ? "ahead" : "behind");
        }
        write_rtc_from_clock();
        ok = true;
    } else {
        ESP_LOGW(TAG, "joined Wi-Fi but NTP did not answer");
    }
    esp_netif_sntp_deinit();
    wifi_sta_leave(); /* the rest of boot never needs the radio */
    s_info.ntp_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    return ok;
}
#endif

void timesync_at_boot(void)
{
    if (pcf85063_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC not answering");
    }
    s_info.rtc_valid = !pcf85063_was_stopped();
    setenv("TZ", CONFIG_DICT_TZ, 1);
    tzset();
#if CONFIG_DICT_NTP
    if (from_ntp()) {
        return;
    }
#endif
    if (from_rtc()) {
        return;
    }
    from_build_time();
    if (clock_is_set()) {
        write_rtc_from_clock(); /* so the next boot at least starts from here */
    }
}

const char *timesync_now_str(char *buf, size_t len)
{
    if (!clock_is_set()) {
        strlcpy(buf, "unset", len);
        return buf;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    return buf;
}

void timesync_info(timesync_info_t *out)
{
    *out = s_info;
}
