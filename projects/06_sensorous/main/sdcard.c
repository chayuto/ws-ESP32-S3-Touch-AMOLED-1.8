#include "sdcard.h"

#include <inttypes.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define POLL_INTERVAL_US (5 * 1000 * 1000)

static bool s_present;
static int64_t s_last_poll_us;
static bool s_check_now;
static uint32_t s_io_errors;
static uint32_t s_total_mb, s_free_mb;
static uint32_t s_mounts;
static bool s_ejected;

/* Retrying every few seconds with no card would spam three lines each time. */
static void quiet_mount_logs(bool quiet)
{
    esp_log_level_t lvl = quiet ? ESP_LOG_NONE : ESP_LOG_INFO;
    esp_log_level_set("sdmmc_common", lvl);
    esp_log_level_set("sdmmc_sd", lvl);
    esp_log_level_set("vfs_fat_sdmmc", lvl);
    /* The BSP's long-filename warning tests a Kconfig choice name and always fires. */
    esp_log_level_set("ESP32-S3-Touch-AMOLED-1.8", quiet ? ESP_LOG_ERROR : ESP_LOG_INFO);
}

static bool try_mount(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = bsp_sdcard_mount();
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s in %lld ms: %s, %llu MB", BSP_SD_MOUNT_POINT, ms, bsp_sdcard->cid.name,
                 ((uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size) / (1024 * 1024));
        s_mounts++;
        /* Before the refresh, not after: sdcard_refresh_space() returns 0/0 unless
         * the card already counts as present, and the callers only assign s_present
         * from this function's return value. Getting this backwards made every
         * record and the card line on the glass say "0 MB free" on a 15 GB card
         * until sdlog's five-minute check happened to run (2026-09-18). */
        s_present = true;
        sdcard_refresh_space();
        return true;
    }
    ESP_LOGD(TAG, "no card (%s, %lld ms)", esp_err_to_name(err), ms);
    return false;
}

static bool still_there(void)
{
    /* CMD13: asks the card for its status. Fails fast if it is gone. */
    return bsp_sdcard != NULL && sdmmc_get_status(bsp_sdcard) == ESP_OK;
}

bool sdcard_init(void)
{
    s_present = try_mount();
    if (!s_present) {
        ESP_LOGW(TAG, "no card at boot; will keep checking every %d s", POLL_INTERVAL_US / 1000000);
    }
    quiet_mount_logs(true);
    s_last_poll_us = esp_timer_get_time();
    return s_present;
}

bool sdcard_poll(void)
{
    if (s_ejected) {
        return false; /* the operator asked for the slot; do not fight them for it */
    }
    int64_t now = esp_timer_get_time();
    if (!s_check_now && now - s_last_poll_us < POLL_INTERVAL_US) {
        return false;
    }
    s_last_poll_us = now;
    s_check_now = false;

    bool was = s_present;
    if (s_present) {
        if (!still_there()) {
            ESP_LOGW(TAG, "card gone");
            bsp_sdcard_unmount(); /* best effort; frees the host so a remount can work */
            s_present = false;
            s_total_mb = s_free_mb = 0;
        }
    } else {
        s_present = try_mount();
    }
    return s_present != was;
}

bool sdcard_present(void)
{
    return s_present;
}

void sdcard_report_io_error(void)
{
    s_io_errors++;
    s_check_now = true;
}

uint32_t sdcard_io_errors(void)
{
    return s_io_errors;
}

void sdcard_space(uint32_t *total_mb, uint32_t *free_mb)
{
    if (total_mb) *total_mb = s_present ? s_total_mb : 0;
    if (free_mb) *free_mb = s_present ? s_free_mb : 0;
}

void sdcard_refresh_space(void)
{
    if (!s_present) {
        s_total_mb = s_free_mb = 0;
        return;
    }
    /* With a stale FSINFO sector this walks the whole FAT, which on a 16 GB card
     * is a few hundred milliseconds. Never call it from the sense loop. */
    uint64_t total = 0, free_b = 0;
    int64_t t0 = esp_timer_get_time();
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        s_total_mb = (uint32_t)(total >> 20);
        s_free_mb = (uint32_t)(free_b >> 20);
        ESP_LOGD(TAG, "%" PRIu32 " of %" PRIu32 " MB free (%lld ms to find out)", s_free_mb, s_total_mb,
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        s_total_mb = s_free_mb = 0;
        ESP_LOGW(TAG, "could not read free space");
    }
}

void sdcard_eject(void)
{
    if (s_present) {
        esp_err_t err = bsp_sdcard_unmount();
        ESP_LOGW(TAG, "ejected: unmount %s. The card is safe to remove.", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "ejected with no card mounted; the slot stays idle until a remount");
    }
    s_present = false;
    s_ejected = true;
    s_total_mb = s_free_mb = 0;
}

bool sdcard_remount(void)
{
    s_ejected = false;
    s_check_now = true;
    quiet_mount_logs(false);
    s_present = try_mount();
    quiet_mount_logs(true);
    if (!s_present) {
        ESP_LOGW(TAG, "remount found no card; polling resumes");
    }
    s_last_poll_us = esp_timer_get_time();
    return s_present;
}

bool sdcard_ejected(void)
{
    return s_ejected;
}

uint32_t sdcard_mounts(void)
{
    return s_mounts;
}
