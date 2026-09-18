#include "blescan.h"

#include <string.h>

#include "census.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "blescan";

/* An unset Kconfig bool is undefined rather than 0, and these are read in plain
 * C expressions as well as in #if. */
#ifndef CONFIG_SENSOROUS_BLE_SCAN_ACTIVE
#define CONFIG_SENSOROUS_BLE_SCAN_ACTIVE 0
#endif

static bool s_synced;
static bool s_scanning;
static uint32_t s_adverts;
static uint32_t s_windows;
static uint32_t s_win_overflow;
static int s_last_devices;

/* Per-window aggregation table, PSRAM. Linear scan: a window holds a couple of
 * hundred devices at most and an advert arrives every few ms, not every few us. */
static blescan_dev_t *s_win;
static int s_win_n;
static SemaphoreHandle_t s_win_lock;
static SemaphoreHandle_t s_done;  /* given on BLE_GAP_EVENT_DISC_COMPLETE */

/*
 * NimBLE reports `type` as public/random; the distinction that matters for
 * location - static random versus a private address that rotates every quarter
 * hour - lives in the top two bits of the most significant address byte.
 * Core spec vol 6, part B, 1.3.2.
 */
static uint8_t classify_addr(uint8_t nimble_type, const uint8_t val[6])
{
    if (nimble_type == BLE_ADDR_PUBLIC || nimble_type == BLE_ADDR_PUBLIC_ID) {
        return 0; /* public */
    }
    switch (val[5] >> 6) {
        case 0x3: return 1; /* static random: stable until the device reboots */
        case 0x1: return 2; /* resolvable private: rotates, worthless for location */
        case 0x0: return 3; /* non-resolvable private */
        default: return 4;  /* reserved */
    }
}

static blescan_dev_t *win_find(const uint8_t mac[6])
{
    for (int i = 0; i < s_win_n; i++) {
        if (memcmp(s_win[i].mac, mac, 6) == 0) {
            return &s_win[i];
        }
    }
    return NULL;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ESP_LOGD(TAG, "DISC_COMPLETE, reason %d", event->disc_complete.reason);
        s_scanning = false;
        xSemaphoreGive(s_done);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    const struct ble_gap_disc_desc *d = &event->disc;
    s_adverts++;

    /* NimBLE hands the address little-endian; every printable form is big-endian. */
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) {
        mac[i] = d->addr.val[5 - i];
    }
    uint8_t addr_type = classify_addr(d->addr.type, d->addr.val);

    char name[32] = {0};
    int32_t company = -1;
    int8_t tx_power = 127;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0) {
        if (f.name != NULL && f.name_len > 0) {
            size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
            memcpy(name, f.name, n);
            /* Names are arbitrary bytes off the air. Anything that would break a
             * JSON line or a log line is replaced here, once, at the edge. */
            for (size_t i = 0; i < n; i++) {
                if (name[i] < 0x20 || name[i] > 0x7e || name[i] == '"' || name[i] == '\\') {
                    name[i] = '.';
                }
            }
        }
        if (f.mfg_data != NULL && f.mfg_data_len >= 2) {
            company = (int32_t)f.mfg_data[0] | ((int32_t)f.mfg_data[1] << 8);
        }
        if (f.tx_pwr_lvl_is_present) {
            tx_power = f.tx_pwr_lvl;
        }
    }

    bool connectable = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                        d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);

    bool is_new = census_ble_seen(mac, addr_type, name[0] ? name : NULL, company, d->rssi, connectable);

    xSemaphoreTake(s_win_lock, portMAX_DELAY);
    blescan_dev_t *e = win_find(mac);
    if (e == NULL) {
        if (s_win_n < CONFIG_SENSOROUS_BLE_WINDOW_DEVS) {
            e = &s_win[s_win_n++];
            memset(e, 0, sizeof(*e));
            memcpy(e->mac, mac, 6);
            e->rssi_best = d->rssi;
            e->company = -1;
            e->tx_power = 127;
            e->is_new = is_new;
        } else {
            s_win_overflow++;
        }
    }
    if (e != NULL) {
        e->addr_type = addr_type;
        e->rssi_last = d->rssi;
        if (d->rssi > e->rssi_best) {
            e->rssi_best = d->rssi;
        }
        e->adverts++;
        e->connectable = e->connectable || connectable;
        if (company >= 0) {
            e->company = company;
        }
        if (tx_power != 127) {
            e->tx_power = tx_power;
        }
        /* A scan response usually carries the complete name where the advert had
         * a shortened one; keep whichever is longer. */
        if (name[0] != '\0' && strlen(name) > strlen(e->name)) {
            strlcpy(e->name, name, sizeof(e->name));
        }
    }
    xSemaphoreGive(s_win_lock);
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGW(TAG, "no usable identity address");
    }
    s_synced = true;
    ESP_LOGI(TAG, "host synced, observer ready (own addr type %u)", own_addr_type);
}

static void on_reset(int reason)
{
    s_synced = false;
    s_scanning = false;
    ESP_LOGE(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();  /* returns when nimble_port_stop() is called */
    nimble_port_freertos_deinit();
}

esp_err_t blescan_init(void)
{
    if (s_win != NULL) {
        return ESP_OK;
    }
    s_win = heap_caps_calloc(CONFIG_SENSOROUS_BLE_WINDOW_DEVS, sizeof(blescan_dev_t), MALLOC_CAP_SPIRAM);
    s_win_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (s_win == NULL || s_win_lock == NULL || s_done == NULL) {
        ESP_LOGE(TAG, "out of memory setting up the window table");
        return ESP_ERR_NO_MEM;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);

    /* The host syncs a few tens of ms after the controller starts. Wait, rather
     * than letting the first window run against a host that is not ready and
     * then reporting an empty environment as if it were quiet. */
    for (int i = 0; i < 200 && !s_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_synced) {
        ESP_LOGE(TAG, "host did not sync within 2 s");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "ready in %lld ms: %s scan, %d ms windows, %d devices per window",
             (esp_timer_get_time() - t0) / 1000, CONFIG_SENSOROUS_BLE_SCAN_ACTIVE ? "active" : "passive",
             CONFIG_SENSOROUS_BLE_WINDOW_MS, CONFIG_SENSOROUS_BLE_WINDOW_DEVS);
    return ESP_OK;
}

bool blescan_ready(void)
{
    return s_synced;
}

int blescan_window(int ms, blescan_dev_cb_t cb, void *ctx)
{
    if (!s_synced || s_win == NULL) {
        ESP_LOGD(TAG, "skipped: host not synced");
        return -1;
    }

    xSemaphoreTake(s_win_lock, portMAX_DELAY);
    s_win_n = 0;
    xSemaphoreGive(s_win_lock);
    xSemaphoreTake(s_done, 0); /* clear any stale completion */

    struct ble_gap_disc_params p = {
        .itvl = 0,   /* controller default: scan interval == window, so it listens continuously */
        .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = CONFIG_SENSOROUS_BLE_SCAN_ACTIVE ? 0 : 1,
        /* Duplicates are what make an RSSI trustworthy: several adverts averaged
         * beats one sample taken at whatever moment the window opened. */
        .filter_duplicates = 0,
    };

    int64_t t0 = esp_timer_get_time();
    /* BLE_HS_FOREVER, and this task ends the window, because NimBLE's own
     * duration timer does not fire here. Measured on hardware 2026-09-18, IDF
     * v5.5.3: ble_gap_disc(..., 4000, ...) logs "duration=4000ms", scans, and is
     * still discovering when asked 5 s later - ble_gap_disc_cancel() returns 0,
     * which it only does for a scan that was actually running. Every window cost
     * a 1 s backstop and a warning. Closing it from here makes the window exactly
     * as long as it says it is, and DISC_COMPLETE is still honoured below in case
     * the host ends the scan by itself (a host reset, say). */
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc: %d", rc);
        return -1;
    }
    s_scanning = true;

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(ms)) != pdTRUE) {
        int crc = ble_gap_disc_cancel();
        if (crc != 0 && crc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "could not stop the scan: rc %d; the radio may still be listening", crc);
        }
        s_scanning = false;
    }

    xSemaphoreTake(s_win_lock, portMAX_DELAY);
    int n = s_win_n;
    int fresh = 0;
    for (int i = 0; i < n; i++) {
        if (s_win[i].is_new) {
            fresh++;
        }
        if (cb != NULL) {
            cb(&s_win[i], ctx);
        }
    }
    xSemaphoreGive(s_win_lock);

    s_windows++;
    s_last_devices = n;
    ESP_LOGI(TAG, "window %u: %d devices in %lld ms, %d new, %u known, %u adverts total",
             (unsigned)s_windows, n, (esp_timer_get_time() - t0) / 1000, fresh,
             (unsigned)census_count(CENSUS_BLE), (unsigned)s_adverts);
    return n;
}

uint32_t blescan_adverts(void)
{
    return s_adverts;
}

uint32_t blescan_windows(void)
{
    return s_windows;
}

int blescan_last_devices(void)
{
    return s_last_devices;
}

uint32_t blescan_window_overflow(void)
{
    return s_win_overflow;
}
