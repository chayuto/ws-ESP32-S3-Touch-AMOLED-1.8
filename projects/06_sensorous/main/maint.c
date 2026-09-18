/*
 * Maintenance HTTP server: the export path.
 *
 * Routes:
 *   GET  /                a one-page dashboard
 *   GET  /api/metrics     heap, uptime, power, card, census sizes
 *   GET  /api/state       what the app thinks it is doing right now
 *   GET  /api/files       every file in the data directory, with sizes
 *   GET  /api/file?name=  one file, whole, streamed. `from=` resumes at a byte offset
 *   GET  /api/log         the flight recorder, tail
 *   GET  /api/census?kind=wifi|ble   the address tables as JSON
 *   POST /api/eject       close the files and unmount, so the card can be pulled
 *   POST /api/mount       remount after a reinsert
 *   POST /api/dump        write the census to radio.jsonl
 *   POST /api/reboot      ask for a restart
 *
 * Read-only apart from those four POSTs, and every one of them is something a
 * person at the board would otherwise do with a button. Nothing uploads.
 */

#include "maint.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "census.h"
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
#include "thermal.h"
#include "wifi_sta.h"

static const char *TAG = "maint";

extern const char maint_html_start[] asm("_binary_maint_html_start");
extern const char maint_html_end[] asm("_binary_maint_html_end");

static httpd_handle_t s_srv;
static maint_state_cb_t s_cb;
static volatile int64_t s_last_req_us;
static volatile maint_req_t s_req;
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
    thermal_status_t th;
    thermal_status(&th);
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    uint32_t rotations = 0, deletions = 0;
    sdlog_maintenance_counts(&rotations, &deletions);
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[1000];
    snprintf(buf, sizeof(buf),
             "{\"uptime_s\":%lld,"
             "\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"psram_free\":%u},"
             "\"app\":{\"version\":\"%s\",\"built\":\"%s %s\"},"
             "\"power\":{\"vbat_mv\":%u,\"batt_pct\":%u,\"vbus\":%s,\"charging\":%s},"
             "\"temp\":{\"chip_c\":%.1f,\"pmu_c\":%.1f,\"board_c\":%.1f,\"level\":\"%s\"},"
             "\"card\":{\"present\":%s,\"ejected\":%s,\"total_mb\":%u,\"free_mb\":%u,\"mounts\":%u,"
             "\"io_errors\":%u,\"rotations\":%u,\"deletions\":%u,\"drops\":%u,\"dir\":\"%s\"},"
             "\"census\":{\"wifi\":%u,\"ble\":%u,\"wifi_cap\":%u,\"ble_cap\":%u,"
             "\"wifi_sightings\":%u,\"ble_sightings\":%u},"
             "\"records\":{\"sensor\":%u,\"radio\":%u,\"event\":%u}}",
             esp_timer_get_time() / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), app->version, app->date, app->time,
             (unsigned)p.vbat_mv, (unsigned)p.batt_pct, p.vbus_in ? "true" : "false",
             p.charging ? "true" : "false", (double)th.chip_c, (double)th.pmu_c, (double)th.board_c,
             thermal_level_name(th.level), sdcard_present() ? "true" : "false",
             sdcard_ejected() ? "true" : "false", (unsigned)total_mb, (unsigned)free_mb,
             (unsigned)sdcard_mounts(), (unsigned)sdcard_io_errors(), (unsigned)rotations, (unsigned)deletions,
             (unsigned)sdlog_dropped(), sdlog_dir(), (unsigned)census_count(CENSUS_WIFI),
             (unsigned)census_count(CENSUS_BLE), (unsigned)census_capacity(CENSUS_WIFI),
             (unsigned)census_capacity(CENSUS_BLE), (unsigned)census_sightings(CENSUS_WIFI),
             (unsigned)census_sightings(CENSUS_BLE), (unsigned)st.records_sensor, (unsigned)st.records_radio,
             (unsigned)st.records_event);
    return send_json(r, buf);
}

static esp_err_t state_get(httpd_req_t *r)
{
    touch();
    maint_app_state_t st = {0};
    if (s_cb) {
        s_cb(&st);
    }
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"scans\":%u,\"wifi_known\":%u,\"ble_known\":%u,"
             "\"wifi_last\":%d,\"ble_last\":%d,\"loop_max_ms\":%u,\"loop_turns\":%u}",
             st.mode ? st.mode : "?", (unsigned)st.scans, (unsigned)st.wifi_known, (unsigned)st.ble_known,
             st.wifi_last, st.ble_last, (unsigned)st.loop_max_ms, (unsigned)st.loop_turns);
    return send_json(r, buf);
}

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A name from a query string is untrusted. Only a plain basename out of the data
 * directory is ever opened: no separators, no dot-dot, nothing that could walk
 * off the directory this server is meant to expose.
 */
static bool safe_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > 63) {
        return false;
    }
    if (strstr(name, "..") != NULL || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return false;
    }
    return true;
}

static esp_err_t files_get(httpd_req_t *r)
{
    touch();
    const char *dir = sdlog_dir();
    if (!sdcard_present() || dir[0] == '\0') {
        return send_json(r, "{\"dir\":\"\",\"files\":[],\"note\":\"no card mounted\"}");
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return send_json(r, "{\"dir\":\"\",\"files\":[],\"note\":\"cannot open the directory\"}");
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    char chunk[400]; /* a FAT long name is up to 255 bytes */
    uint32_t total_mb = 0, free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    snprintf(chunk, sizeof(chunk), "{\"dir\":\"%s\",\"free_mb\":%u,\"total_mb\":%u,\"files\":[", dir,
             (unsigned)free_mb, (unsigned)total_mb);
    httpd_resp_send_chunk(r, chunk, HTTPD_RESP_USE_STRLEN);

    struct dirent *e;
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        char path[384];
        struct stat sb;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            continue;
        }
        snprintf(chunk, sizeof(chunk), "%s{\"name\":\"%s\",\"bytes\":%ld}", first ? "" : ",", e->d_name,
                 (long)sb.st_size);
        httpd_resp_send_chunk(r, chunk, HTTPD_RESP_USE_STRLEN);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(r, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/*
 * Stream one file whole. `from=` resumes at a byte offset, which is what makes a
 * 60 MB file recoverable over a flaky link: the client asks again from where it
 * stopped instead of starting over. The card reads at a few hundred KB/s over
 * SDMMC 1-bit, so this is minutes, not seconds, for a long run.
 */
static esp_err_t file_get(httpd_req_t *r)
{
    touch();
    char query[128] = {0};
    char name[64] = {0};
    char from_s[24] = {0};
    long from = 0;
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
        if (httpd_query_key_value(query, "from", from_s, sizeof(from_s)) == ESP_OK) {
            from = strtol(from_s, NULL, 10);
        }
    }
    if (!safe_name(name)) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "name must be a plain file name in the data directory");
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", sdlog_dir(), name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        /* Say which errno it was. ENFILE is not a missing file - it is the mount
         * out of descriptors - and answering "no such file" to it sent a whole
         * extraction run looking for the wrong fault (2026-09-18). */
        int e = errno;
        char msg[192];
        ESP_LOGW(TAG, "cannot open %s: %s (errno %d)", path, strerror(e), e);
        snprintf(msg, sizeof(msg), "cannot open %s: %s (errno %d)", name, strerror(e), e);
        httpd_resp_set_status(r, e == ENOENT ? "404 Not Found" : "500 Internal Server Error");
        return httpd_resp_sendstr(r, msg);
    }
    if (from > 0) {
        fseek(f, from, SEEK_SET);
    }
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    /* 2 KB at a time: bigger reads do not go faster on a 1-bit SDMMC bus and the
     * buffer is on this task's stack. */
    static char buf[2048];
    size_t n;
    long sent = 0;
    int64_t t0 = esp_timer_get_time();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) {
            fclose(f);
            ESP_LOGW(TAG, "%s: client went away after %ld bytes", name, sent);
            return ESP_FAIL;
        }
        sent += n;
    }
    fclose(f);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s: sent %ld bytes from offset %ld in %d ms (%ld KB/s)", name, sent, from, ms,
             ms > 0 ? sent / ms : 0);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* The flight recorder's tail, for a quick look without downloading the whole file. */
static esp_err_t log_get(httpd_req_t *r)
{
    touch();
    const char *path = sdlog_path();
    FILE *f = (path && path[0]) ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        httpd_resp_set_status(r, "404 Not Found");
        return httpd_resp_sendstr(r, "no log file open");
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    const long want = 32768;
    fseek(f, size > want ? size - want : 0, SEEK_SET);
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

/* The census straight out of RAM - available even with no card in the slot. */
static esp_err_t census_get(httpd_req_t *r)
{
    touch();
    char query[64] = {0}, kind_s[8] = {0};
    census_kind_t kind = CENSUS_WIFI;
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "kind", kind_s, sizeof(kind_s));
    }
    if (strcmp(kind_s, "ble") == 0) {
        kind = CENSUS_BLE;
    }

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char chunk[360];
    snprintf(chunk, sizeof(chunk), "{\"kind\":\"%s\",\"count\":%u,\"rows\":[",
             kind == CENSUS_BLE ? "ble" : "wifi", (unsigned)census_count(kind));
    httpd_resp_send_chunk(r, chunk, HTTPD_RESP_USE_STRLEN);

    size_t n = census_count(kind);
    for (size_t i = 0; i < n; i++) {
        census_row_t row;
        if (!census_row(kind, i, &row)) {
            break;
        }
        char mac[18];
        census_mac_str(row.mac, mac, sizeof(mac));
        /* SSIDs and names are escaped when they are written to the card; here the
         * only consumer is the dashboard, so the same rule applies - send the
         * address and the numbers, and the text only after the same scrubbing. */
        char text[40] = {0};
        const char *src = (kind == CENSUS_WIFI) ? row.ssid : row.name;
        for (size_t k = 0, o = 0; src[k] && o < sizeof(text) - 1; k++) {
            char c = src[k];
            text[o++] = (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\') ? c : '.';
        }
        if (kind == CENSUS_WIFI) {
            snprintf(chunk, sizeof(chunk),
                     "%s{\"macAddress\":\"%s\",\"ssid\":\"%s\",\"channel\":%u,\"signalStrength\":%d,"
                     "\"rssi_best\":%d,\"sightings\":%u,\"last_up_ms\":%u}",
                     i ? "," : "", mac, text, (unsigned)row.channel, row.rssi_last, row.rssi_best,
                     (unsigned)row.sightings, (unsigned)row.last_up_ms);
        } else {
            snprintf(chunk, sizeof(chunk),
                     "%s{\"macAddress\":\"%s\",\"addrType\":\"%s\",\"name\":\"%s\",\"company\":%d,"
                     "\"signalStrength\":%d,\"rssi_best\":%d,\"sightings\":%u,\"last_up_ms\":%u}",
                     i ? "," : "", mac, census_addr_type_name(row.addr_type), text, (int)row.company,
                     row.rssi_last, row.rssi_best, (unsigned)row.sightings, (unsigned)row.last_up_ms);
        }
        httpd_resp_send_chunk(r, chunk, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(r, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* The four things a client may ask the board to do                          */
/* ------------------------------------------------------------------------- */

static esp_err_t request_post(httpd_req_t *r, maint_req_t what, const char *name)
{
    touch();
    s_req = what;
    ESP_LOGW(TAG, "%s requested over HTTP", name);
    return send_json(r, "{\"ok\":true,\"note\":\"the board acts on this in its next loop turn\"}");
}

static esp_err_t eject_post(httpd_req_t *r) { return request_post(r, MAINT_REQ_EJECT, "eject"); }
static esp_err_t mount_post(httpd_req_t *r) { return request_post(r, MAINT_REQ_MOUNT, "mount"); }
static esp_err_t dump_post(httpd_req_t *r) { return request_post(r, MAINT_REQ_CENSUS, "census dump"); }
static esp_err_t reboot_post(httpd_req_t *r) { return request_post(r, MAINT_REQ_REBOOT, "reboot"); }

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
     * reduced socket pool - which is what happened on the first run in 05. */
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    /* File streaming runs on this task and reads into a 2 KB buffer. */
    cfg.stack_size = 8192;
    /* A whole-file download of tens of MB over SDMMC takes minutes. The default
     * 5 s send timeout would kill it partway and leave a truncated file that
     * looks complete. */
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 30;
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
        {.uri = "/api/files", .method = HTTP_GET, .handler = files_get},
        {.uri = "/api/file", .method = HTTP_GET, .handler = file_get},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/api/census", .method = HTTP_GET, .handler = census_get},
        {.uri = "/api/eject", .method = HTTP_POST, .handler = eject_post},
        {.uri = "/api/mount", .method = HTTP_POST, .handler = mount_post},
        {.uri = "/api/dump", .method = HTTP_POST, .handler = dump_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_srv, &routes[i]);
    }
    touch();
    ESP_LOGW(TAG, "maintenance up at http://%s/ - radio is ON and scanning is stopped", s_ip);
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
    ESP_LOGW(TAG, "maintenance down, scanning resumes");
}

bool maint_active(void) { return s_srv != NULL; }

int maint_idle_s(void)
{
    if (s_last_req_us == 0) {
        return 0;
    }
    return (int)((esp_timer_get_time() - s_last_req_us) / 1000000);
}

maint_req_t maint_take_request(void)
{
    maint_req_t v = s_req;
    s_req = MAINT_REQ_NONE;
    return v;
}

const char *maint_ip(void) { return s_ip; }
