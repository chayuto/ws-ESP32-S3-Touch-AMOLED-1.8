#include "sdlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
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

static void ring_push(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    size_t used = (s_head + RING_BYTES - s_tail) % RING_BYTES;
    if (n >= RING_BYTES - used - 1) {
        s_dropped++;
    } else {
        for (size_t i = 0; i < n; i++) {
            s_ring[s_head] = s[i];
            s_head = (s_head + 1) % RING_BYTES;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static int hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_console(fmt, args); /* console first, exactly as before */

    if (s_ring != NULL && !xPortInIsrContext()) {
        char line[LOG_LINE_MAX];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0) {
            if (n >= (int)sizeof(line)) {
                n = sizeof(line) - 1;
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
        if (s_file == NULL) {
            continue;
        }
        if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        bool failed = false;
        for (;;) {
            size_t n = 0;
            portENTER_CRITICAL(&s_lock);
            while (n < sizeof(chunk) && s_tail != s_head) {
                chunk[n++] = s_ring[s_tail];
                s_tail = (s_tail + 1) % RING_BYTES;
            }
            portEXIT_CRITICAL(&s_lock);
            if (n == 0) {
                break;
            }
            if (s_file != NULL && fwrite(chunk, 1, n, s_file) != n) {
                failed = true;
                break;
            }
            s_written += n;
        }
        if (!failed && s_file != NULL && esp_timer_get_time() - last_sync > SYNC_MS * 1000) {
            last_sync = esp_timer_get_time();
            if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
                failed = true;
            }
        }
        if (failed && s_file != NULL) {
            fclose(s_file);
            s_file = NULL;
            xSemaphoreGive(s_file_mutex);
            ESP_LOGW(TAG, "write to %s failed; log file closed", s_path);
            sdcard_report_io_error();
            continue;
        }
        xSemaphoreGive(s_file_mutex);
    }
}

void sdlog_init(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
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
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        xSemaphoreGive(s_file_mutex);
        ESP_LOGW(TAG, "cannot open %s for append", path);
        return ESP_FAIL;
    }
    strlcpy(s_path, path, sizeof(s_path));
    const esp_app_desc_t *app = esp_app_get_description();
    fprintf(f, "\n===== boot: %s %s (IDF %s), reset reason %d, uptime %lld ms, ring dropped %u =====\n",
            app->project_name, app->version, app->idf_ver, (int)esp_reset_reason(),
            (long long)(esp_timer_get_time() / 1000), (unsigned)s_dropped);
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
