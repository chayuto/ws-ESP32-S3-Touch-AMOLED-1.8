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

/* Mounted here rather than with bsp_sdcard_mount(), for one reason: the BSP
 * hard-codes max_files = 5 and this project holds exactly five files open all
 * the time - sensorous.log plus the four record channels. That leaves no
 * descriptor for anything else, so the export path could list the files and
 * then fail to open any of them: /api/file answered "no such file" for a file
 * the same request had just listed (measured 2026-09-18, first extraction run).
 * Nothing else about the mount differs from the BSP's - the pins, the 1-bit
 * SDMMC width and the allocation unit are copied from it. */
#define SDCARD_MAX_FILES 10

static bool try_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = SDCARD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    int64_t t0 = esp_timer_get_time();
    /* bsp_sdcard is the BSP's own handle; the rest of this file and the BSP's
     * unmount both read it, so it stays the one place the card lives. */
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config,
                                            &bsp_sdcard);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s in %lld ms: %s, %llu MB, up to %d files open at once",
                 BSP_SD_MOUNT_POINT, ms, bsp_sdcard->cid.name,
                 ((uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size) / (1024 * 1024),
                 SDCARD_MAX_FILES);
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
