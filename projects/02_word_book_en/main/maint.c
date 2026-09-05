#include "maint.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "pmu.h"
#include "recognizer.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "timesync.h"
#include "wifi_sta.h"

static const char *TAG = "maint";

/* Ping a host a few times and log the outcome: tells wired-vs-wireless isolation
 * from a dead link when a laptop cannot reach the board. */
static void probe(const char *host)
{
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target;
    if (!ipaddr_aton(host, &target)) {
        return;
    }
    pc.target_addr = target;
    pc.count = 3;
    pc.interval_ms = 300;
    pc.timeout_ms = 800;
    esp_ping_handle_t h;
    if (esp_ping_new_session(&pc, NULL, &h) != ESP_OK) {
        return;
    }
    esp_ping_start(h);
    vTaskDelay(pdMS_TO_TICKS(3 * 300 + 900));
    uint32_t sent = 0, recv = 0, rtt = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    esp_ping_delete_session(h);
    ESP_LOGI(TAG, "ping %s: %lu/%lu replies%s", host, (unsigned long)recv, (unsigned long)sent, recv ? "" : " - unreachable from the board");
}

#define BOOK_DIR BSP_SD_MOUNT_POINT "/book"

extern const char maint_html_start[] asm("_binary_maint_html_start");
extern const char maint_html_end[] asm("_binary_maint_html_end");

static httpd_handle_t s_httpd;
static maint_state_cb_t s_state_cb;
static int64_t s_last_req_us;
static volatile bool s_book_changed, s_reboot;
static char s_ip[16];

static void touch(void)
{
    s_last_req_us = esp_timer_get_time();
}

/* Filenames we accept on the card: letters, digits, _ - . and one of our extensions. */
static bool safe_name(const char *n)
{
    size_t len = strlen(n);
    if (len < 5 || len > 40 || n[0] == '.' || strstr(n, "..")) {
        return false;
    }
    for (const char *p = n; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
    }
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".wav") ||
                   !strcasecmp(dot, ".rgb565") || !strcasecmp(dot, ".json"));
}

static const char *name_from_uri(httpd_req_t *req, const char *prefix, char *out, size_t len)
{
    const char *n = req->uri + strlen(prefix);
    if (strlen(n) >= len) {
        return NULL;
    }
    strlcpy(out, n, len);
    char *q = strchr(out, '?');
    if (q) {
        *q = '\0';
    }
    return safe_name(out) ? out : NULL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

/* Drop a JPEG's cached conversion so the next showing re-decodes. */
static void drop_cache(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !strcasecmp(dot, ".rgb565")) {
        return;
    }
    char path[112];
    snprintf(path, sizeof(path), "%s/%.*s.rgb565", BOOK_DIR, (int)(dot - name), name);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%.*s.fit.rgb565", BOOK_DIR, (int)(dot - name), name);
    unlink(path);
}

/* ---- handlers ---- */

static esp_err_t h_index(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, maint_html_start, maint_html_end - maint_html_start - 1);
}

static esp_err_t h_metrics(httpd_req_t *req)
{
    touch();
    maint_app_state_t st = {0};
    if (s_state_cb) {
        s_state_cb(&st);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    char now[32];
    float avg, peak;
    int speech;
    recognizer_mic_level(&avg, &peak, &speech);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", app->project_name);
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddStringToObject(o, "idf", app->idf_ver);
    cJSON_AddStringToObject(o, "time", timesync_now_str(now, sizeof(now)));
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "internal_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(o, "card", sdcard_present());
    cJSON_AddBoolToObject(o, "files_ok", st.files_ok);
    cJSON_AddBoolToObject(o, "log_active", sdlog_active());
    cJSON_AddNumberToObject(o, "log_bytes", (double)sdlog_size());
    cJSON_AddBoolToObject(o, "classifier_active", sdlog_aux_active(0));
    cJSON_AddNumberToObject(o, "classifier_bytes", (double)sdlog_aux_size(0));
    cJSON_AddBoolToObject(o, "state_active", sdlog_aux_active(1));
    cJSON_AddNumberToObject(o, "state_bytes", (double)sdlog_aux_size(1));
    cJSON_AddNumberToObject(o, "words", st.words);
    cJSON_AddNumberToObject(o, "heard", st.heard);
    cJSON_AddStringToObject(o, "last_word", st.last_word ? st.last_word : "");
    cJSON_AddNumberToObject(o, "last_prob", st.last_prob);
    cJSON_AddNumberToObject(o, "mic_avg_dbfs", avg);
    cJSON_AddNumberToObject(o, "mic_peak_dbfs", peak);
    cJSON_AddNumberToObject(o, "mic_speech_pct", speech);
    pmu_status_t ps = {0};
    pmu_read(&ps);
    cJSON_AddBoolToObject(o, "usb", ps.vbus_in);
    cJSON_AddBoolToObject(o, "battery", ps.batt_present);
    cJSON_AddNumberToObject(o, "vbat_mv", ps.vbat_mv);
    cJSON_AddNumberToObject(o, "vbus_mv", ps.vbus_mv);
    cJSON_AddNumberToObject(o, "vsys_mv", ps.vsys_mv);
    cJSON_AddNumberToObject(o, "batt_pct", ps.batt_pct);
    cJSON_AddBoolToObject(o, "charging", ps.charging);
    cJSON_AddNumberToObject(o, "min_prob_pct", CONFIG_WORDBOOK_MIN_PROB_PCT);
    cJSON_AddNumberToObject(o, "sound_prob_pct", CONFIG_WORDBOOK_SOUND_PROB_PCT);
    return send_json(req, o);
}

static esp_err_t h_book_list(httpd_req_t *req)
{
    touch();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "card", sdcard_present());
    cJSON *files = cJSON_AddArrayToObject(o, "files");
    DIR *d = opendir(BOOK_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char path[112];
            struct stat st;
            if (strlen(e->d_name) > 60 || e->d_name[0] == '.') {
                continue; /* macOS ._sidecars and the like */
            }
            strlcpy(path, BOOK_DIR "/", sizeof(path));
            strlcat(path, e->d_name, sizeof(path));
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", e->d_name);
            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
            cJSON_AddNumberToObject(f, "mtime", (double)st.st_mtime);
            cJSON_AddItemToArray(files, f);
        }
        closedir(d);
    }
    return send_json(req, o);
}

static esp_err_t h_book_get(httpd_req_t *req)
{
    touch();
    char name[48];
    if (!name_from_uri(req, "/api/book/", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[112];
    snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    }
    httpd_resp_set_type(req, strstr(name, ".wav") ? "audio/wav" : strstr(name, ".jp") ? "image/jpeg" : "application/octet-stream");
    char *buf = malloc(4096);
    size_t n;
    while (buf && (n = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_book_put(httpd_req_t *req)
{
    touch();
    char name[48];
    if (!name_from_uri(req, "/api/book/", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name must be word.jpg / word.wav, letters digits _ - only");
    }
    if (!sdcard_present()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    }
    if (req->content_len == 0 || req->content_len > 12 * 1024 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size must be 1 byte .. 12 MB");
    }
    mkdir(BOOK_DIR, 0777);
    char tmp[112], path[112];
    snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
    snprintf(tmp, sizeof(tmp), "%s/upload.tmp", BOOK_DIR);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write to card");
    }
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    int64_t t0 = esp_timer_get_time();
    while (ok && left > 0) {
        int n = httpd_req_recv(req, buf, left < 8192 ? left : 8192);
        if (n <= 0) {
            ok = false;
            break;
        }
        ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
        left -= (size_t)n;
    }
    free(buf);
    fclose(f);
    if (!ok) {
        unlink(tmp);
        sdcard_report_io_error();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    drop_cache(name);
    ESP_LOGI(TAG, "uploaded %s (%u bytes in %lld ms)", path, (unsigned)req->content_len,
             (long long)((esp_timer_get_time() - t0) / 1000));
    s_book_changed = true;
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t h_book_delete(httpd_req_t *req)
{
    touch();
    char name[48];
    if (!name_from_uri(req, "/api/book/", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[112];
    snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
    if (unlink(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    }
    drop_cache(name);
    ESP_LOGI(TAG, "deleted %s", path);
    s_book_changed = true;
    return httpd_resp_sendstr(req, "ok");
}

/* GET /api/log            whole file
 * GET /api/log?tail=N     last N lines */
static esp_err_t h_log_get(httpd_req_t *req)
{
    touch();
    const char *path = sdlog_path();
    if (!path[0]) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no log (card?)");
    }
    char q[16] = {0};
    int tail = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "tail", v, sizeof(v)) == ESP_OK) {
            tail = atoi(v);
        }
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open log");
    }
    httpd_resp_set_type(req, "text/plain");
    if (tail <= 0) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=02_word_book_en.log");
    }
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    if (tail > 0) {
        /* Walk back from the end until we have `tail` newlines, then stream from there. */
        fseek(f, 0, SEEK_END);
        long pos = ftell(f), start = 0;
        int lines = 0;
        while (pos > 0 && lines <= tail) {
            long chunk = pos < 8192 ? pos : 8192;
            pos -= chunk;
            fseek(f, pos, SEEK_SET);
            size_t n = fread(buf, 1, (size_t)chunk, f);
            for (long i = (long)n - 1; i >= 0; i--) {
                if (buf[i] == '\n' && ++lines > tail) {
                    start = pos + i + 1;
                    break;
                }
            }
        }
        fseek(f, start, SEEK_SET);
    }
    size_t n;
    while ((n = fread(buf, 1, 8192, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_aux(httpd_req_t *req, int ch, const char *dl_name);

static esp_err_t h_clog_get(httpd_req_t *req)
{
    return send_aux(req, 0, "02_word_book_en.classifier.jsonl");
}

static esp_err_t h_state_get(httpd_req_t *req)
{
    return send_aux(req, 1, "02_word_book_en.state.jsonl");
}

static esp_err_t send_aux(httpd_req_t *req, int ch, const char *dl_name)
{
    touch();
    const char *path = sdlog_aux_path(ch);
    if (!path[0]) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no record file (card?)");
    }
    char q[16] = {0};
    int tail = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "tail", v, sizeof(v)) == ESP_OK) {
            tail = atoi(v);
        }
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open");
    }
    httpd_resp_set_type(req, "application/x-ndjson");
    char dl[96];
    snprintf(dl, sizeof(dl), "attachment; filename=%s", dl_name);
    if (tail <= 0) {
        httpd_resp_set_hdr(req, "Content-Disposition", dl);
    }
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    if (tail > 0) {
        fseek(f, 0, SEEK_END);
        long pos = ftell(f), start = 0;
        int lines = 0;
        while (pos > 0 && lines <= tail) {
            long chunk = pos < 8192 ? pos : 8192;
            pos -= chunk;
            fseek(f, pos, SEEK_SET);
            size_t n = fread(buf, 1, (size_t)chunk, f);
            for (long i = (long)n - 1; i >= 0; i--) {
                if (buf[i] == '\n' && ++lines > tail) {
                    start = pos + i + 1;
                    break;
                }
            }
        }
        fseek(f, start, SEEK_SET);
    }
    size_t n;
    while ((n = fread(buf, 1, 8192, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_clog_delete(httpd_req_t *req)
{
    touch();
    return sdlog_aux_truncate(0) == ESP_OK ? httpd_resp_sendstr(req, "ok")
                                            : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no classifier log");
}

static esp_err_t h_state_delete(httpd_req_t *req)
{
    touch();
    return sdlog_aux_truncate(1) == ESP_OK ? httpd_resp_sendstr(req, "ok")
                                            : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no state log");
}

static esp_err_t h_log_delete(httpd_req_t *req)
{
    touch();
    return sdlog_truncate() == ESP_OK ? httpd_resp_sendstr(req, "ok")
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no log to truncate");
}

static esp_err_t h_reload(httpd_req_t *req)
{
    touch();
    s_book_changed = true;
    ESP_LOGI(TAG, "reload requested");
    return httpd_resp_sendstr(req, "ok: the book is rescanned when maintenance mode ends");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    touch();
    ESP_LOGW(TAG, "reboot requested over HTTP");
    s_reboot = true;
    return httpd_resp_sendstr(req, "rebooting");
}

/* ---- lifecycle ---- */

esp_err_t maint_start(maint_state_cb_t cb)
{
    if (s_httpd) {
        return ESP_OK;
    }
    s_state_cb = cb;
    if (!wifi_sta_join(20)) {
        return ESP_FAIL;
    }
    wifi_sta_connected(s_ip, sizeof(s_ip));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.stack_size = 6144;
    hc.max_open_sockets = 4; /* LWIP_MAX_SOCKETS (8) minus the 3 httpd keeps for itself, minus one spare */
    hc.max_uri_handlers = 16;
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 30;
    hc.send_wait_timeout = 30;
    esp_err_t err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) {
        wifi_sta_leave();
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/metrics", .method = HTTP_GET, .handler = h_metrics},
        {.uri = "/api/book", .method = HTTP_GET, .handler = h_book_list},
        {.uri = "/api/book/*", .method = HTTP_GET, .handler = h_book_get},
        {.uri = "/api/book/*", .method = HTTP_PUT, .handler = h_book_put},
        {.uri = "/api/book/*", .method = HTTP_DELETE, .handler = h_book_delete},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log_get},
        {.uri = "/api/log", .method = HTTP_DELETE, .handler = h_log_delete},
        {.uri = "/api/classifier", .method = HTTP_GET, .handler = h_clog_get},
        {.uri = "/api/classifier", .method = HTTP_DELETE, .handler = h_clog_delete},
        {.uri = "/api/state", .method = HTTP_GET, .handler = h_state_get},
        {.uri = "/api/state", .method = HTTP_DELETE, .handler = h_state_delete},
        {.uri = "/api/reload", .method = HTTP_POST, .handler = h_reload},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    touch();
    ESP_LOGI(TAG, "maintenance mode: http://%s/  (idle timeout %d min)", s_ip, CONFIG_WORDBOOK_MAINT_IDLE_MIN);
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK) {
        char gw[16];
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ip.gw));
        probe(gw);
    }
    if (strlen(CONFIG_WORDBOOK_PROBE_HOST)) {
        probe(CONFIG_WORDBOOK_PROBE_HOST);
    }
    return ESP_OK;
}

void maint_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    wifi_sta_leave();
    s_ip[0] = '\0';
    ESP_LOGI(TAG, "maintenance mode off");
}

bool maint_active(void)
{
    return s_httpd != NULL;
}

int maint_idle_s(void)
{
    return (int)((esp_timer_get_time() - s_last_req_us) / 1000000);
}

bool maint_take_book_changed(void)
{
    bool c = s_book_changed;
    s_book_changed = false;
    return c;
}

bool maint_take_reboot(void)
{
    bool r = s_reboot;
    s_reboot = false;
    return r;
}

const char *maint_ip(void)
{
    return s_ip;
}
