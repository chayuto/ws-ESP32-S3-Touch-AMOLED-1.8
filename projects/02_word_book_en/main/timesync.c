#include "timesync.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "pcf85063.h"
#include "sdkconfig.h"

#if CONFIG_WORDBOOK_NTP
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "wifi_sta.h"
#endif

static const char *TAG = "time";

#define VALID_AFTER_YEAR 2025

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
    setenv("TZ", is_utc ? "UTC0" : CONFIG_WORDBOOK_TZ, 1);
    tzset();
    copy.tm_isdst = -1;
    time_t secs = mktime(&copy);
    struct timeval tv = {.tv_sec = secs};
    settimeofday(&tv, NULL);
    setenv("TZ", CONFIG_WORDBOOK_TZ, 1);
    tzset();
    char buf[32];
    ESP_LOGI(TAG, "clock set from %s: %s", source, timesync_now_str(buf, sizeof(buf)));
}

static bool from_rtc(void)
{
    struct tm t;
    if (pcf85063_get(&t) != ESP_OK || t.tm_year + 1900 < VALID_AFTER_YEAR) {
        return false;
    }
    set_clock(&t, true, "RTC");
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

#if CONFIG_WORDBOOK_NTP
static bool from_ntp(void)
{
    if (!wifi_sta_join(CONFIG_WORDBOOK_NTP_TIMEOUT_S)) {
        return false;
    }
    bool ok = false;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_WORDBOOK_NTP_SERVER);
    esp_netif_sntp_init(&sc);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        setenv("TZ", CONFIG_WORDBOOK_TZ, 1);
        tzset();
        char buf[32];
        ESP_LOGI(TAG, "clock set from NTP (%s): %s", CONFIG_WORDBOOK_NTP_SERVER, timesync_now_str(buf, sizeof(buf)));
        write_rtc_from_clock();
        ok = true;
    } else {
        ESP_LOGW(TAG, "joined Wi-Fi but NTP did not answer");
    }
    esp_netif_sntp_deinit();
    wifi_sta_leave(); /* the rest of boot never needs the radio */
    return ok;
}
#endif

void timesync_at_boot(void)
{
    if (pcf85063_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC not answering");
    }
    setenv("TZ", CONFIG_WORDBOOK_TZ, 1);
    tzset();
#if CONFIG_WORDBOOK_NTP
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
