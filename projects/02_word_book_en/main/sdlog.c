#include "sdlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "timesync.h"

static const char *TAG = "sdlog";

#define RING_BYTES   (64 * 1024)
#define LOG_LINE_MAX     512
#define DRAIN_MS     250
#define SYNC_MS      2000

static vprintf_like_t s_console;
static char *s_ring;
static volatile size_t s_head, s_tail; /* head: next write; tail: next read */
static size_t s_dropped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static FILE *s_file;
static SemaphoreHandle_t s_file_mutex;
static char s_path[96];
static size_t s_written;

/* Aux channel: own ring, same drain task and mutex. */
#define AUX_RING_BYTES (32 * 1024)
static char *s_aux_ring;
static volatile size_t s_aux_head, s_aux_tail;
static size_t s_aux_dropped;
static FILE *s_aux_file;
static char s_aux_path[96];

static void push(char *ring, size_t cap, volatile size_t *head, volatile size_t *tail, size_t *dropped,
                 const char *s, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    size_t used = (*head + cap - *tail) % cap;
    if (n >= cap - used - 1) {
        (*dropped)++;
    } else {
        for (size_t i = 0; i < n; i++) {
            ring[*head] = s[i];
            *head = (*head + 1) % cap;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static void ring_push(const char *s, size_t n)
{
    push(s_ring, RING_BYTES, &s_head, &s_tail, &s_dropped, s, n);
}

/* Move up to sizeof(chunk) bytes out of a ring; returns the count. */
static size_t pull(char *ring, size_t cap, volatile size_t *head, volatile size_t *tail, char *chunk, size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_lock);
    while (n < max && *tail != *head) {
        chunk[n++] = ring[*tail];
        *tail = (*tail + 1) % cap;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

/* Drain one ring into one file. Returns false on a write failure. */
static bool drain(char *ring, size_t cap, volatile size_t *head, volatile size_t *tail, FILE *f, char *chunk,
                  size_t chunk_len, size_t *written)
{
    for (;;) {
        size_t n = pull(ring, cap, head, tail, chunk, chunk_len);
        if (n == 0) {
            return true;
        }
        if (fwrite(chunk, 1, n, f) != n) {
            return false;
        }
        *written += n;
    }
}

/* One static line buffer: esp_log serialises vprintf calls under its own lock, and
 * a stack buffer here would land on every logging task's stack - the 2 KB system
 * event task overflowed on it once DEBUG lines were compiled in. */
static char s_line[LOG_LINE_MAX];

static int hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_console(fmt, args); /* console first, exactly as before */

    if (s_ring != NULL && !xPortInIsrContext()) {
        char *line = s_line;
        int n = vsnprintf(line, LOG_LINE_MAX, fmt, copy);
        if (n > 0) {
            if (n >= LOG_LINE_MAX) {
                n = LOG_LINE_MAX - 1;
            }
            ring_push(line, (size_t)n);
        }
    }
    va_end(copy);
    return ret;
}

static void drain_task(void *arg)
{
    (void)arg;
    char chunk[1024];
    int64_t last_sync = 0;
    size_t aux_written = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
        if (s_file == NULL && s_aux_file == NULL) {
            continue;
        }
        if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        bool failed = false;
        if (s_file && !drain(s_ring, RING_BYTES, &s_head, &s_tail, s_file, chunk, sizeof(chunk), &s_written)) {
            failed = true;
        }
        if (s_aux_file && s_aux_ring &&
            !drain(s_aux_ring, AUX_RING_BYTES, &s_aux_head, &s_aux_tail, s_aux_file, chunk, sizeof(chunk), &aux_written)) {
            failed = true;
        }
        if (!failed && esp_timer_get_time() - last_sync > SYNC_MS * 1000) {
            last_sync = esp_timer_get_time();
            if (s_file && (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0)) {
                failed = true;
            }
            if (s_aux_file && (fflush(s_aux_file) != 0 || fsync(fileno(s_aux_file)) != 0)) {
                failed = true;
            }
        }
        if (failed) {
            if (s_file) {
                fclose(s_file);
                s_file = NULL;
            }
            if (s_aux_file) {
                fclose(s_aux_file);
                s_aux_file = NULL;
            }
            xSemaphoreGive(s_file_mutex);
            ESP_LOGW(TAG, "write to the card failed; log files closed");
            sdcard_report_io_error();
            continue;
        }
        xSemaphoreGive(s_file_mutex);
    }
}

void sdlog_init(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    s_aux_ring = heap_caps_malloc(AUX_RING_BYTES, MALLOC_CAP_SPIRAM);
    s_file_mutex = xSemaphoreCreateMutex();
    if (s_ring == NULL || s_file_mutex == NULL) {
        ESP_LOGE(TAG, "no memory for the log ring; SD logging off");
        return;
    }
    s_console = esp_log_set_vprintf(hook);
    xTaskCreatePinnedToCore(drain_task, "sdlog", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "capturing log lines (%d KB ring); file opens when a card is mounted", RING_BYTES / 1024);
}

esp_err_t sdlog_open(const char *path)
{
    if (s_ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_file != NULL) {
        xSemaphoreGive(s_file_mutex);
        return ESP_OK;
    }
    /* Rotate: over the cap, the old file becomes <path>.1 (replacing the previous .1). */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (long)CONFIG_WORDBOOK_LOG_MAX_KB * 1024) {
        char old[112];
        snprintf(old, sizeof(old), "%s.1", path);
        unlink(old);
        if (rename(path, old) == 0) {
            ESP_LOGI(TAG, "rotated %s (%ld bytes) to %s", path, (long)st.st_size, old);
        }
    }
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        xSemaphoreGive(s_file_mutex);
        ESP_LOGW(TAG, "cannot open %s for append", path);
        return ESP_FAIL;
    }
    strlcpy(s_path, path, sizeof(s_path));
    const esp_app_desc_t *app = esp_app_get_description();
    char now[32];
    fprintf(f, "\n===== boot: %s %s (IDF %s), reset reason %d, uptime %lld ms, time %s, ring dropped %u =====\n",
            app->project_name, app->version, app->idf_ver, (int)esp_reset_reason(),
            (long long)(esp_timer_get_time() / 1000), timesync_now_str(now, sizeof(now)), (unsigned)s_dropped);
    s_file = f;
    xSemaphoreGive(s_file_mutex);
    ESP_LOGI(TAG, "appending to %s", path);
    return ESP_OK;
}

void sdlog_close(void)
{
    if (s_file_mutex == NULL || xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (s_file != NULL) {
        fclose(s_file); /* may fail if the card is already gone; nothing to do about it */
        s_file = NULL;
        ESP_LOGI(TAG, "log file closed (%u bytes written this session)", (unsigned)s_written);
    }
    xSemaphoreGive(s_file_mutex);
}

bool sdlog_active(void)
{
    return s_file != NULL;
}

const char *sdlog_path(void)
{
    return s_path;
}

long sdlog_size(void)
{
    struct stat st;
    return (s_path[0] && stat(s_path, &st) == 0) ? (long)st.st_size : 0;
}

esp_err_t sdlog_truncate(void)
{
    if (!s_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[96];
    strlcpy(path, s_path, sizeof(path));
    sdlog_close();
    unlink(path);
    char old[112];
    snprintf(old, sizeof(old), "%s.1", path);
    unlink(old);
    esp_err_t err = sdlog_open(path);
    ESP_LOGI(TAG, "log truncated");
    return err;
}

static void rotate(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (long)CONFIG_WORDBOOK_LOG_MAX_KB * 1024) {
        char old[112];
        snprintf(old, sizeof(old), "%s.1", path);
        unlink(old);
        if (rename(path, old) == 0) {
            ESP_LOGI(TAG, "rotated %s (%ld bytes) to %s", path, (long)st.st_size, old);
        }
    }
}

esp_err_t sdlog_aux_open(const char *path)
{
    if (s_aux_ring == NULL || s_file_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_aux_file != NULL) {
        xSemaphoreGive(s_file_mutex);
        return ESP_OK;
    }
    rotate(path);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        xSemaphoreGive(s_file_mutex);
        ESP_LOGW(TAG, "cannot open %s for append", path);
        return ESP_FAIL;
    }
    strlcpy(s_aux_path, path, sizeof(s_aux_path));
    s_aux_file = f;
    xSemaphoreGive(s_file_mutex);
    ESP_LOGI(TAG, "appending records to %s", path);
    return ESP_OK;
}

void sdlog_aux_close(void)
{
    if (s_file_mutex == NULL || xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (s_aux_file != NULL) {
        fclose(s_aux_file);
        s_aux_file = NULL;
        ESP_LOGI(TAG, "record file closed");
    }
    xSemaphoreGive(s_file_mutex);
}

void sdlog_aux_write(const char *line)
{
    if (s_aux_ring == NULL) {
        return;
    }
    size_t n = strlen(line);
    push(s_aux_ring, AUX_RING_BYTES, &s_aux_head, &s_aux_tail, &s_aux_dropped, line, n);
    push(s_aux_ring, AUX_RING_BYTES, &s_aux_head, &s_aux_tail, &s_aux_dropped, "\n", 1);
}

bool sdlog_aux_active(void)
{
    return s_aux_file != NULL;
}

const char *sdlog_aux_path(void)
{
    return s_aux_path;
}

long sdlog_aux_size(void)
{
    struct stat st;
    return (s_aux_path[0] && stat(s_aux_path, &st) == 0) ? (long)st.st_size : 0;
}

esp_err_t sdlog_aux_truncate(void)
{
    if (!s_aux_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[96];
    strlcpy(path, s_aux_path, sizeof(path));
    sdlog_aux_close();
    unlink(path);
    char old[112];
    snprintf(old, sizeof(old), "%s.1", path);
    unlink(old);
    return sdlog_aux_open(path);
}
