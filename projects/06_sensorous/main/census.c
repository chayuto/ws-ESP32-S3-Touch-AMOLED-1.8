#include "census.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "census";

/*
 * Open-addressed index over the rows, so a busy BLE window does not turn into a
 * linear scan of a thousand rows per advert. Sized to twice the capacity and
 * rounded up to a power of two, which keeps the load factor under 0.5 and the
 * probe short. Key: the low four MAC bytes, which are the varying ones.
 */
typedef struct {
    census_row_t *rows;
    size_t cap, n;
    int32_t *index;   /* -1 empty, otherwise a row number */
    size_t index_mask;
    uint32_t overflow;
    uint32_t sightings;
} table_t;

static table_t s_tab[2];
static SemaphoreHandle_t s_lock;

static size_t next_pow2(size_t v)
{
    size_t p = 8;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static bool table_init(table_t *t, size_t cap)
{
    t->cap = cap;
    t->n = 0;
    t->rows = heap_caps_calloc(cap, sizeof(census_row_t), MALLOC_CAP_SPIRAM);
    t->index_mask = next_pow2(cap * 2) - 1;
    t->index = heap_caps_malloc((t->index_mask + 1) * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (t->rows == NULL || t->index == NULL) {
        return false;
    }
    memset(t->index, 0xff, (t->index_mask + 1) * sizeof(int32_t)); /* all -1 */
    return true;
}

static uint32_t mac_hash(const uint8_t mac[6])
{
    uint32_t h = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    h ^= (uint32_t)mac[0] << 7;
    h ^= (uint32_t)mac[1] << 15;
    /* fmix32 from MurmurHash3 - cheap, and it spreads the OUI-heavy top bytes. */
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* Caller holds the lock. Returns the row, or NULL when full. `is_new` says which. */
static census_row_t *upsert(table_t *t, const uint8_t mac[6], bool *is_new)
{
    uint32_t slot = mac_hash(mac) & t->index_mask;
    for (size_t probe = 0; probe <= t->index_mask; probe++) {
        int32_t row = t->index[slot];
        if (row < 0) {
            if (t->n >= t->cap) {
                t->overflow++;
                return NULL;
            }
            int32_t nr = (int32_t)t->n++;
            t->index[slot] = nr;
            memset(&t->rows[nr], 0, sizeof(t->rows[nr]));
            memcpy(t->rows[nr].mac, mac, 6);
            t->rows[nr].company = -1;
            *is_new = true;
            return &t->rows[nr];
        }
        if (memcmp(t->rows[row].mac, mac, 6) == 0) {
            *is_new = false;
            return &t->rows[row];
        }
        slot = (slot + 1) & t->index_mask;
    }
    t->overflow++;
    return NULL;
}

static void stamp(census_row_t *r, int8_t rssi, bool is_new)
{
    int64_t now = (int64_t)time(NULL);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    /* Before the clock is set, time() returns a small number. Store it anyway and
     * let the reader judge; up_ms is the honest one. */
    if (is_new) {
        r->first_s = now;
        r->first_up_ms = up;
        r->rssi_best = rssi;
    } else if (rssi > r->rssi_best) {
        r->rssi_best = rssi;
    }
    r->last_s = now;
    r->last_up_ms = up;
    r->rssi_last = rssi;
    r->sightings++;
}

bool census_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return false;
    }
    if (!table_init(&s_tab[CENSUS_WIFI], CONFIG_SENSOROUS_CENSUS_WIFI_MAX) ||
        !table_init(&s_tab[CENSUS_BLE], CONFIG_SENSOROUS_CENSUS_BLE_MAX)) {
        ESP_LOGE(TAG, "PSRAM would not give up the tables");
        vSemaphoreDelete(s_lock);
        s_lock = NULL; /* the mutex is the "census is alive" flag every entry point checks */
        s_tab[CENSUS_WIFI].n = s_tab[CENSUS_BLE].n = 0;
        return false;
    }
    ESP_LOGI(TAG, "wifi table %d rows (%u B), ble table %d rows (%u B), both in PSRAM",
             CONFIG_SENSOROUS_CENSUS_WIFI_MAX,
             (unsigned)(CONFIG_SENSOROUS_CENSUS_WIFI_MAX * sizeof(census_row_t)),
             CONFIG_SENSOROUS_CENSUS_BLE_MAX,
             (unsigned)(CONFIG_SENSOROUS_CENSUS_BLE_MAX * sizeof(census_row_t)));
    return true;
}

bool census_wifi_seen(const uint8_t mac[6], const char *ssid, uint8_t channel, int8_t rssi, uint8_t authmode)
{
    if (s_lock == NULL) {
        return false; /* census_init() failed; the app was told and kept going */
    }
    bool is_new = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tab[CENSUS_WIFI].sightings++;
    census_row_t *r = upsert(&s_tab[CENSUS_WIFI], mac, &is_new);
    if (r != NULL) {
        stamp(r, rssi, is_new);
        r->channel = channel;
        r->authmode = authmode;
        if (ssid != NULL && ssid[0] != '\0') {
            strlcpy(r->ssid, ssid, sizeof(r->ssid));
            r->hidden = false;
        } else if (is_new) {
            r->hidden = true;
        }
    }
    xSemaphoreGive(s_lock);
    return is_new;
}

bool census_ble_seen(const uint8_t mac[6], uint8_t addr_type, const char *name, int32_t company, int8_t rssi,
                     bool connectable)
{
    if (s_lock == NULL) {
        return false;
    }
    bool is_new = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tab[CENSUS_BLE].sightings++;
    census_row_t *r = upsert(&s_tab[CENSUS_BLE], mac, &is_new);
    if (r != NULL) {
        stamp(r, rssi, is_new);
        r->addr_type = addr_type;
        r->connectable = connectable;
        if (company >= 0) {
            r->company = company;
        }
        /* A shortened name arrives in some adverts and the complete one in the
         * scan response; keep the longer. */
        if (name != NULL && name[0] != '\0' && strlen(name) > strlen(r->name)) {
            strlcpy(r->name, name, sizeof(r->name));
        }
    }
    xSemaphoreGive(s_lock);
    return is_new;
}

size_t census_count(census_kind_t kind)
{
    return s_tab[kind].n;
}

size_t census_capacity(census_kind_t kind)
{
    return s_tab[kind].cap;
}

uint32_t census_overflow(census_kind_t kind)
{
    return s_tab[kind].overflow;
}

uint32_t census_sightings(census_kind_t kind)
{
    return s_tab[kind].sightings;
}

bool census_row(census_kind_t kind, size_t i, census_row_t *out)
{
    if (s_lock == NULL) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (i < s_tab[kind].n) {
        *out = s_tab[kind].rows[i];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

const char *census_mac_str(const uint8_t mac[6], char *buf, size_t n)
{
    snprintf(buf, n, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

const char *census_addr_type_name(uint8_t addr_type)
{
    switch (addr_type) {
        case 0: return "public";
        case 1: return "random_static";
        case 2: return "rpa";      /* rotates; useless for location */
        case 3: return "nrpa";
        default: return "unknown";
    }
}

void census_reset(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < 2; k++) {
        s_tab[k].n = 0;
        s_tab[k].overflow = 0;
        s_tab[k].sightings = 0;
        memset(s_tab[k].index, 0xff, (s_tab[k].index_mask + 1) * sizeof(int32_t));
    }
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "census cleared - a new survey starts here");
}
