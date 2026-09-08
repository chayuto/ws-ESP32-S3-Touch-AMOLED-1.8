#include "wifiscan.h"

#include <string.h>

#include "census.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

static const char *TAG = "wifiscan";

/* See blescan.c: an unset Kconfig bool is undefined, not 0. */
#ifndef CONFIG_SENSOROUS_WIFI_SCAN_ACTIVE
#define CONFIG_SENSOROUS_WIFI_SCAN_ACTIVE 0
#endif

/* A dense urban sweep tops out well under this; the driver truncates rather than fails. */
#define MAX_APS 96

static wifi_ap_record_t *s_recs;
static int s_last_ms;
static int s_last_count;
static uint32_t s_cycles;

esp_err_t wifiscan_init(void)
{
    if (s_recs != NULL) {
        return ESP_OK;
    }
    s_recs = heap_caps_malloc(MAX_APS * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (s_recs == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %d records (%u B)", MAX_APS, (unsigned)(MAX_APS * sizeof(wifi_ap_record_t)));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready: %s scan, %d ms dwell, up to %d APs per sweep",
             CONFIG_SENSOROUS_WIFI_SCAN_ACTIVE ? "active" : "passive", CONFIG_SENSOROUS_WIFI_DWELL_MS, MAX_APS);
    return ESP_OK;
}

int wifiscan_run(wifiscan_ap_cb_t cb, void *ctx)
{
    if (s_recs == NULL || !wifi_radio_is_up()) {
        ESP_LOGD(TAG, "skipped: %s", s_recs == NULL ? "not initialised" : "radio down");
        return -1;
    }

    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,        /* every channel the country code allows */
        .show_hidden = true, /* a hidden AP still has a BSSID, and that is the useful part */
#if CONFIG_SENSOROUS_WIFI_SCAN_ACTIVE
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = CONFIG_SENSOROUS_WIFI_DWELL_MS / 3, .max = CONFIG_SENSOROUS_WIFI_DWELL_MS},
#else
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = CONFIG_SENSOROUS_WIFI_DWELL_MS,
#endif
    };

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_last_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed after %d ms: %s", s_last_ms, esp_err_to_name(err));
        return -1;
    }

    uint16_t n = MAX_APS;
    err = esp_wifi_scan_get_ap_records(&n, s_recs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_ap_records: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return -1;
    }

    int fresh = 0;
    for (uint16_t i = 0; i < n; i++) {
        const wifi_ap_record_t *r = &s_recs[i];
        wifiscan_ap_t ap = {
            .channel = r->primary,
            .rssi = r->rssi,
            .authmode = (uint8_t)r->authmode,
        };
        memcpy(ap.bssid, r->bssid, 6);
        strlcpy(ap.ssid, (const char *)r->ssid, sizeof(ap.ssid));
        ap.is_new = census_wifi_seen(ap.bssid, ap.ssid, ap.channel, ap.rssi, ap.authmode);
        if (ap.is_new) {
            fresh++;
        }
        if (cb != NULL) {
            cb(&ap, ctx);
        }
    }

    s_cycles++;
    s_last_count = n;
    ESP_LOGI(TAG, "sweep %u: %u APs in %d ms, %d new, %u known", (unsigned)s_cycles, (unsigned)n, s_last_ms, fresh,
             (unsigned)census_count(CENSUS_WIFI));
    if (n == MAX_APS) {
        ESP_LOGW(TAG, "result buffer full at %d - there may be more APs than were reported", MAX_APS);
    }
    return n;
}

int wifiscan_last_ms(void)
{
    return s_last_ms;
}

uint32_t wifiscan_cycles(void)
{
    return s_cycles;
}

int wifiscan_last_count(void)
{
    return s_last_count;
}

const char *wifiscan_auth_name(uint8_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa";
        case WIFI_AUTH_WPA2_PSK: return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_ENTERPRISE: return "enterprise";
        case WIFI_AUTH_WPA3_PSK: return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK: return "wapi";
        case WIFI_AUTH_OWE: return "owe";
        default: return "other";
    }
}
