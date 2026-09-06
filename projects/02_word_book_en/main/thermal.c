#include "thermal.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pmu.h"
#include "sdkconfig.h"

static const char *TAG = "thermal";

#define SAMPLE_US (1000 * 1000)
#define WARM_C    ((float)CONFIG_WORDBOOK_THERMAL_WARM_C)
#define HOT_C     ((float)CONFIG_WORDBOOK_THERMAL_HOT_C)
#define TRIP_C    ((float)CONFIG_WORDBOOK_THERMAL_TRIP_C)
#define HYST_C    ((float)CONFIG_WORDBOOK_THERMAL_HYST_C)
#define NVS_NS    "wordbook"
#define NVS_KEY   "trip"

typedef struct {
    int64_t when;
    uint32_t up_s;
    float chip_c, pmu_c;
    uint32_t count;
} marker_t;

static temperature_sensor_handle_t s_tsens;
static thermal_status_t s_last = {.chip_c = NAN, .pmu_c = NAN, .board_c = NAN};
static int64_t s_last_sample_us = -SAMPLE_US;
static thermal_level_t s_wanted;
static int s_agree;
static float s_sim_c = NAN;
static bool s_nvs_ready, s_have_marker, s_disagree_warned;
static marker_t s_marker;

static const char *const s_names[] = {"ok", "warm", "hot", "trip"};

const char *thermal_level_name(thermal_level_t level)
{
    return s_names[level & 3];
}

static bool sane(float c)
{
    return !isnan(c) && c > -20.0f && c < 130.0f;
}

static float read_chip(void)
{
    float c = NAN;
    if (s_tsens != NULL && temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return NAN;
    }
    return c;
}

/* The level these degrees ask for, given the current one: up at the line, down only HYST below it. */
static thermal_level_t wanted_level(float c, thermal_level_t cur)
{
    thermal_level_t up = c >= TRIP_C ? THERMAL_TRIP : c >= HOT_C ? THERMAL_HOT : c >= WARM_C ? THERMAL_WARM : THERMAL_OK;
    if (up > cur) {
        return up;
    }
    thermal_level_t down = c >= TRIP_C - HYST_C ? THERMAL_TRIP
                           : c >= HOT_C - HYST_C ? THERMAL_HOT
                           : c >= WARM_C - HYST_C ? THERMAL_WARM
                                                  : THERMAL_OK;
    return down < cur ? down : cur;
}

void thermal_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100); /* +-2 C over the range that matters */
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chip temperature sensor: %s; the PMU's die is the only reading", esp_err_to_name(err));
        s_tsens = NULL;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_ready = err == ESP_OK;
    nvs_handle_t h;
    if (s_nvs_ready && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_marker);
        s_have_marker = nvs_get_blob(h, NVS_KEY, &s_marker, &n) == ESP_OK && n == sizeof(s_marker);
        nvs_close(h);
    }
    if (s_have_marker) {
        ESP_LOGW(TAG, "a previous run powered the board off for heat: chip %.1f C, PMU %.1f C at uptime %" PRIu32 " s, trip %" PRIu32
                      " on this board (time %lld)",
                 s_marker.chip_c, s_marker.pmu_c, s_marker.up_s, s_marker.count, (long long)s_marker.when);
    }

    thermal_status_t st;
    thermal_poll(&st);
    ESP_LOGI(TAG, "guard on: chip %.1f C, PMU %.1f C; warm %d, hot %d, trip %d C on the hotter die, %d s to change level, %d C hysteresis",
             st.chip_c, st.pmu_c, CONFIG_WORDBOOK_THERMAL_WARM_C, CONFIG_WORDBOOK_THERMAL_HOT_C, CONFIG_WORDBOOK_THERMAL_TRIP_C,
             CONFIG_WORDBOOK_THERMAL_SAMPLES, CONFIG_WORDBOOK_THERMAL_HYST_C);
}

bool thermal_poll(thermal_status_t *out)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_sample_us < SAMPLE_US) {
        if (out) *out = s_last;
        return false;
    }
    s_last_sample_us = now;

    thermal_status_t st = s_last;
    st.chip_c = read_chip();
    st.pmu_c = pmu_die_temp_c();
    float board = NAN;
    if (sane(st.chip_c)) {
        board = st.chip_c;
    }
    if (sane(st.pmu_c) && (isnan(board) || st.pmu_c > board)) {
        board = st.pmu_c;
    }
    st.simulated = !isnan(s_sim_c);
    if (st.simulated) {
        board = s_sim_c;
    }
    st.board_c = board;

    if (sane(st.chip_c) && sane(st.pmu_c) && fabsf(st.chip_c - st.pmu_c) > 25.0f && !s_disagree_warned) {
        s_disagree_warned = true;
        ESP_LOGW(TAG, "the two dies disagree by more than 25 C (chip %.1f, PMU %.1f): one sensor may be off; the hotter one rules",
                 st.chip_c, st.pmu_c);
    }

    bool changed = false;
    if (!isnan(board)) {
        thermal_level_t want = wanted_level(board, st.level);
        if (want != st.level) {
            if (want == s_wanted) {
                s_agree++;
            } else {
                s_wanted = want;
                s_agree = 1;
            }
            if (s_agree >= CONFIG_WORDBOOK_THERMAL_SAMPLES) {
                st.level = want;
                s_agree = 0;
                changed = true;
            }
        } else {
            s_agree = 0;
        }
    }
    st.hot_s = st.level >= THERMAL_HOT ? st.hot_s + 1 : 0;
    s_last = st;
    if (out) *out = st;
    return changed;
}

void thermal_status(thermal_status_t *out)
{
    *out = s_last;
}

void thermal_mark_trip(const thermal_status_t *st)
{
    marker_t m = {.when = (int64_t)time(NULL),
                  .up_s = (uint32_t)(esp_timer_get_time() / 1000000),
                  .chip_c = st->chip_c,
                  .pmu_c = st->pmu_c,
                  .count = (s_have_marker ? s_marker.count : 0) + 1};
    nvs_handle_t h;
    if (!s_nvs_ready || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no NVS: the trip will not be remembered across the power-off");
        return;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &m, sizeof(m));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "trip marker %" PRIu32 " written (%s)", m.count, esp_err_to_name(err));
    s_marker = m;
    s_have_marker = true;
}

bool thermal_last_trip(thermal_trip_t *out)
{
    if (!s_have_marker) {
        return false;
    }
    out->when = s_marker.when;
    out->up_s = s_marker.up_s;
    out->chip_c = s_marker.chip_c;
    out->pmu_c = s_marker.pmu_c;
    out->count = s_marker.count;
    return true;
}

void thermal_simulate_step(void)
{
    if (isnan(s_sim_c)) {
        s_sim_c = WARM_C + 1;
    } else if (s_sim_c < HOT_C) {
        s_sim_c = HOT_C + 1;
    } else if (s_sim_c < TRIP_C) {
        s_sim_c = TRIP_C + 1;
    } else {
        s_sim_c = NAN;
    }
    if (isnan(s_sim_c)) {
        ESP_LOGW(TAG, "simulation off: real readings again (levels drop after %d s below the line minus %d C)",
                 CONFIG_WORDBOOK_THERMAL_SAMPLES, CONFIG_WORDBOOK_THERMAL_HYST_C);
    } else {
        ESP_LOGW(TAG, "SIMULATING a board at %.0f C (real: chip %.1f, PMU %.1f); the trip level is a dry run, it never powers off",
                 s_sim_c, s_last.chip_c, s_last.pmu_c);
    }
}
