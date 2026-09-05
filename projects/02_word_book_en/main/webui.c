#include "webui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdcard.h"
#include "sdkconfig.h"

static const char *TAG = "webui";

#define BOOK_DIR   BSP_SD_MOUNT_POINT "/book"
#define PHOTO_BYTES (368 * 448 * 2)

extern const char webui_html_start[] asm("_binary_webui_html_start");
extern const char webui_html_end[] asm("_binary_webui_html_end");

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static bool s_wifi_inited;
static volatile bool s_changed;

/* ---- words.json read/modify/write, on the file, not the live book ---- */

static cJSON *manifest_load(void)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/words.json", BOOK_DIR);
    FILE *f = fopen(path, "rb");
    cJSON *root = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 64 * 1024) {
            char *buf = malloc((size_t)n + 1);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                buf[n] = '\0';
                root = cJSON_Parse(buf);
            }
            free(buf);
        }
        fclose(f);
    }
    if (root == NULL) {
        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "format", 1);
        cJSON_AddItemToObject(root, "words", cJSON_CreateArray());
    }
    if (!cJSON_IsArray(cJSON_GetObjectItem(root, "words"))) {
        cJSON_DeleteItemFromObject(root, "words");
        cJSON_AddItemToObject(root, "words", cJSON_CreateArray());
    }
    return root;
}

static esp_err_t manifest_save(cJSON *root)
{
    mkdir(BOOK_DIR, 0777);
    char tmp[96], path[96];
    snprintf(tmp, sizeof(tmp), "%s/words.tmp", BOOK_DIR);
    snprintf(path, sizeof(path), "%s/words.json", BOOK_DIR);
    char *text = cJSON_Print(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        free(text);
        return ESP_FAIL;
    }
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    free(text);
    if (!ok) {
        return ESP_FAIL;
    }
    unlink(path);
    return rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;
}

static cJSON *manifest_find(cJSON *root, const char *word)
{
    cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItem(root, "words")) {
        cJSON *t = cJSON_GetObjectItem(w, "text");
        if (cJSON_IsString(t) && strcasecmp(t->valuestring, word) == 0) {
            return w;
        }
    }
    return NULL;
}

static void lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s += 'a' - 'A';
        }
    }
}

static bool valid_word(const char *w)
{
    size_t n = strlen(w);
    if (n < 2 || n >= BOOK_TEXT_LEN) {
        return false;
    }
    for (; *w; w++) {
        if (!((*w >= 'A' && *w <= 'Z') || (*w >= 'a' && *w <= 'z') || *w == ' ')) {
            return false;
        }
    }
    return true;
}

/* ---- handlers ---- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, webui_html_start, webui_html_end - webui_html_start - 1);
}

static esp_err_t h_book(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "card", sdcard_present());
    cJSON *arr = cJSON_AddArrayToObject(out, "words");
    if (sdcard_present()) {
        cJSON *root = manifest_load();
        cJSON *w;
        cJSON_ArrayForEach(w, cJSON_GetObjectItem(root, "words")) {
            cJSON *t = cJSON_GetObjectItem(w, "text");
            if (!cJSON_IsString(t)) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "text", t->valuestring);
            cJSON_AddBoolToObject(o, "photo", cJSON_IsString(cJSON_GetObjectItem(w, "photo")));
            cJSON_AddBoolToObject(o, "prompt", cJSON_IsString(cJSON_GetObjectItem(w, "prompt")));
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_Delete(root);
    }
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static const char *word_from_uri(httpd_req_t *req, const char *prefix, char *out, size_t len)
{
    const char *w = req->uri + strlen(prefix);
    if (strlen(w) == 0 || strlen(w) >= len) {
        return NULL;
    }
    strlcpy(out, w, len);
    char *q = strchr(out, '?');
    if (q) {
        *q = '\0';
    }
    return valid_word(out) ? out : NULL;
}

static esp_err_t h_photo_get(httpd_req_t *req)
{
    char word[BOOK_TEXT_LEN];
    if (!word_from_uri(req, "/api/photo/", word, sizeof(word))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad word");
    }
    lower(word);
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.rgb565", BOOK_DIR, word);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no photo");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char *buf = malloc(4096);
    size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_photo_put(httpd_req_t *req)
{
    char word[BOOK_TEXT_LEN];
    if (!word_from_uri(req, "/api/photo/", word, sizeof(word))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad word");
    }
    if (!sdcard_present()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    }
    if (req->content_len != PHOTO_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected 368x448 RGB565, 329728 bytes");
    }
    char upper[BOOK_TEXT_LEN], file[BOOK_TEXT_LEN];
    strlcpy(upper, word, sizeof(upper));
    for (char *p = upper; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 'a' - 'A';
        }
    }
    strlcpy(file, word, sizeof(file));
    lower(file);

    mkdir(BOOK_DIR, 0777);
    char tmp[96], path[96];
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", BOOK_DIR, file);
    snprintf(path, sizeof(path), "%s/%s.rgb565", BOOK_DIR, file);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write to card");
    }
    char *buf = malloc(4096);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    while (ok && left > 0) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
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

    /* Make sure the word is in the manifest and points at the file. */
    cJSON *root = manifest_load();
    cJSON *w = manifest_find(root, upper);
    if (w == NULL) {
        w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "text", upper);
        cJSON_AddItemToArray(cJSON_GetObjectItem(root, "words"), w);
    }
    char rel[48];
    snprintf(rel, sizeof(rel), "%s.rgb565", file);
    cJSON_DeleteItemFromObject(w, "photo");
    cJSON_AddStringToObject(w, "photo", rel);
    esp_err_t err = manifest_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest write failed");
    }
    ESP_LOGI(TAG, "photo saved: %s", path);
    s_changed = true;
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t h_word_post(httpd_req_t *req)
{
    char word[BOOK_TEXT_LEN] = {0};
    int n = httpd_req_recv(req, word, sizeof(word) - 1);
    if (n <= 0 || !valid_word(word)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "letters only, 2-23 chars");
    }
    if (!sdcard_present()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    }
    for (char *p = word; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 'a' - 'A';
        }
    }
    cJSON *root = manifest_load();
    if (manifest_find(root, word) == NULL) {
        if (cJSON_GetArraySize(cJSON_GetObjectItem(root, "words")) >= BOOK_MAX_WORDS) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "book is full");
        }
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "text", word);
        cJSON_AddItemToArray(cJSON_GetObjectItem(root, "words"), w);
    }
    esp_err_t err = manifest_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest write failed");
    }
    ESP_LOGI(TAG, "word added: %s", word);
    s_changed = true;
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t h_word_delete(httpd_req_t *req)
{
    char word[BOOK_TEXT_LEN];
    if (!word_from_uri(req, "/api/word/", word, sizeof(word))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad word");
    }
    if (!sdcard_present()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    }
    cJSON *root = manifest_load();
    cJSON *arr = cJSON_GetObjectItem(root, "words");
    int idx = 0;
    cJSON *w;
    cJSON_ArrayForEach(w, arr) {
        cJSON *t = cJSON_GetObjectItem(w, "text");
        if (cJSON_IsString(t) && strcasecmp(t->valuestring, word) == 0) {
            cJSON_DeleteItemFromArray(arr, idx);
            break;
        }
        idx++;
    }
    esp_err_t err = manifest_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest write failed");
    }
    ESP_LOGI(TAG, "word removed: %s (files kept)", word);
    s_changed = true;
    return httpd_resp_sendstr(req, "ok");
}

/* ---- lifecycle ---- */

esp_err_t webui_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }
    if (!s_wifi_inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_ap_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_WORDBOOK_WIFI_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_WORDBOOK_WIFI_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_WORDBOOK_WIFI_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = strlen(CONFIG_WORDBOOK_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.stack_size = 8192;
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "httpd_start");
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/book", .method = HTTP_GET, .handler = h_book},
        {.uri = "/api/photo/*", .method = HTTP_GET, .handler = h_photo_get},
        {.uri = "/api/photo/*", .method = HTTP_PUT, .handler = h_photo_put},
        {.uri = "/api/word", .method = HTTP_POST, .handler = h_word_post},
        {.uri = "/api/word/*", .method = HTTP_DELETE, .handler = h_word_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    ESP_LOGI(TAG, "setup mode: join Wi-Fi '%s', open http://192.168.4.1", CONFIG_WORDBOOK_WIFI_SSID);
    return ESP_OK;
}

void webui_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "setup mode off");
}

bool webui_take_book_changed(void)
{
    bool c = s_changed;
    s_changed = false;
    return c;
}
