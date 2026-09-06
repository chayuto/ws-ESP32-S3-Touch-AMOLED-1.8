/*
 * Maintenance HTTP server. Read-only by design: this project has nothing on the
 * card worth uploading, and the fewer write paths the radio exposes, the better.
 *
 * Routes:
 *   GET /              a one-page dashboard
 *   GET /api/metrics   heap, uptime, power, card, MX-1 tallies
 *   GET /api/state     what the app thinks it is doing right now
 *   GET /api/log       the flight recorder, tail
 *   GET /api/decodes   the decode records, tail
 *   POST /api/reboot   ask for a restart
 */

#include "maint.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "wifi_sta.h"

static const char *TAG = "maint";

extern const char maint_html_start[] asm("_binary_maint_html_start");
extern const char maint_html_end[] asm("_binary_maint_html_end");

static httpd_handle_t s_srv;
static maint_state_cb_t s_cb;
static volatile int64_t s_last_req_us;
static volatile bool s_reboot;
static char s_ip[16] = "-";

static void touch(void) { s_last_req_us = esp_timer_get_time(); }

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

static esp_err_t root_get(httpd_req_t *r)
{
    touch();
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, maint_html_start, maint_html_end - maint_html_start - 1);
}

static esp_err_t metrics_get(httpd_req_t *r)
{
    touch();
    maint_app_state_t st = {0};
    if (s_cb) {
        s_cb(&st);
    }
    pmu_status_t p = {0};
    pmu_read(&p);
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[900];
    snprintf(buf, sizeof(buf),
             "{\"uptime_s\":%lld,"
             "\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"psram_free\":%u},"
             "\"app\":{\"version\":\"%s\",\"built\":\"%s %s\"},"
             "\"power\":{\"vbat_mv\":%u,\"batt_pct\":%u,\"vbus\":%s,\"charging\":%s,\"pmu_c\":%.1f},"
             "\"card\":{\"total_mb\":%u,\"free_mb\":%u},"
             "\"mx1\":{\"results\":%u,\"raw_nonempty\":%u,\"raw_differs\":%u},"
             "\"utterances\":%u}",
             esp_timer_get_time() / 1000000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), app->version, app->date, app->time,
             (unsigned)p.vbat_mv, (unsigned)p.batt_pct, p.vbus_in ? "true" : "false",
             p.charging ? "true" : "false", pmu_die_temp_c(), (unsigned)total_mb, (unsigned)free_mb,
             (unsigned)st.probe_results, (unsigned)st.probe_raw, (unsigned)st.probe_differs,
             (unsigned)st.utterances);
    return send_json(r, buf);
}

static esp_err_t state_get(httpd_req_t *r)
{
    touch();
    maint_app_state_t st = {0};
    if (s_cb) {
        s_cb(&st);
    }
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"last_text\":\"%s\",\"last_prob\":%.3f,\"utterances\":%u,"
             "\"loop_max_ms\":%u,\"loop_turns\":%u,\"lang\":\"%s\"}",
             st.last_text ? st.last_text : "", st.last_prob, (unsigned)st.utterances, (unsigned)st.loop_max_ms,
             (unsigned)st.loop_turns,
#if CONFIG_DICT_LANG_CN
             "cn"
#else
             "en"
#endif
    );
    return send_json(r, buf);
}

/* Stream the tail of a file on the card. */
static esp_err_t send_tail(httpd_req_t *r, const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_set_status(r, "404 Not Found");
        return httpd_resp_sendstr(r, "no such file");
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)want ? size - (long)want : 0;
    fseek(f, from, SEEK_SET);
    httpd_resp_set_type(r, "text/plain");
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(r, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t log_get(httpd_req_t *r)
{
    touch();
    return send_tail(r, "/sdcard/05_dictation.log", 32768);
}

static esp_err_t decodes_get(httpd_req_t *r)
{
    touch();
    return send_tail(r, "/sdcard/05_dictation.decodes.jsonl", 32768);
}

static esp_err_t reboot_post(httpd_req_t *r)
{
    touch();
    s_reboot = true;
    ESP_LOGW(TAG, "reboot requested over HTTP");
    return send_json(r, "{\"ok\":true}");
}

esp_err_t maint_start(maint_state_cb_t cb)
{
    if (s_srv != NULL) {
        return ESP_OK;
    }
    s_cb = cb;
    if (!wifi_sta_join(20)) {
        ESP_LOGE(TAG, "wifi join failed; maintenance not started");
        return ESP_ERR_TIMEOUT;
    }
    if (!wifi_sta_connected(s_ip, sizeof(s_ip))) {
        snprintf(s_ip, sizeof(s_ip), "?");
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* LWIP_MAX_SOCKETS (8) minus the 3 httpd keeps for itself, minus one spare.
     * The default of 7 fails httpd_start() with ESP_ERR_INVALID_ARG against our
     * reduced socket pool - which is what happened on the first run here. */
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    esp_err_t err = httpd_start(&s_srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd: %s", esp_err_to_name(err));
        wifi_sta_leave();
        return err;
    }
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/metrics", .method = HTTP_GET, .handler = metrics_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/api/decodes", .method = HTTP_GET, .handler = decodes_get},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_srv, &routes[i]);
    }
    touch();
    ESP_LOGW(TAG, "maintenance up at http://%s/ - radio is ON, audio still never leaves the board", s_ip);
    return ESP_OK;
}

void maint_stop(void)
{
    if (s_srv == NULL) {
        return;
    }
    httpd_stop(s_srv);
    s_srv = NULL;
    wifi_sta_leave();
    snprintf(s_ip, sizeof(s_ip), "-");
    ESP_LOGW(TAG, "maintenance down, radio off");
}

bool maint_active(void) { return s_srv != NULL; }

int maint_idle_s(void)
{
    if (s_last_req_us == 0) {
        return 0;
    }
    return (int)((esp_timer_get_time() - s_last_req_us) / 1000000);
}

bool maint_take_reboot(void)
{
    bool v = s_reboot;
    s_reboot = false;
    return v;
}

const char *maint_ip(void) { return s_ip; }
