/*
 * bringup-01 — first-boot validation for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
 *
 * Prints what the silicon actually reports, scans the shared I2C bus, names the
 * devices found, and infers the board revision from the touch controller
 * address (V1 = FT3168 @ 0x38, V2 = CST820 @ 0x15). Run this before writing any
 * peripheral code: the two revisions ship different display and touch chips.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "bringup";

#define I2C_PROBE_TIMEOUT_MS 50

#define ADDR_TOUCH_CST820 0x15 /* V2 board */
#define ADDR_ES8311       0x18
#define ADDR_AXP2101      0x34
#define ADDR_TOUCH_FT3168 0x38 /* V1 board */
#define ADDR_TCA9554      0x20
#define ADDR_PCF85063     0x51
#define ADDR_QMI8658      0x6B

typedef struct {
    uint8_t addr;
    const char *name;
} i2c_device_t;

static const i2c_device_t k_known_devices[] = {
    {ADDR_TOUCH_CST820, "CST820 capacitive touch (V2 board)"},
    {ADDR_ES8311, "ES8311 audio codec"},
    {ADDR_AXP2101, "AXP2101 PMU"},
    {ADDR_TCA9554, "TCA9554 I/O expander"},
    {ADDR_TOUCH_FT3168, "FT3168 capacitive touch (V1 board)"},
    {ADDR_PCF85063, "PCF85063 RTC"},
    {ADDR_QMI8658, "QMI8658 6-axis IMU"},
};

static const char *device_name(uint8_t addr)
{
    for (size_t i = 0; i < sizeof(k_known_devices) / sizeof(k_known_devices[0]); i++) {
        if (k_known_devices[i].addr == addr) {
            return k_known_devices[i].name;
        }
    }
    return "unknown";
}

static void print_chip_features(uint32_t features)
{
    printf("Features:");
    printf("%s", (features & CHIP_FEATURE_WIFI_BGN) ? " Wi-Fi" : "");
    printf("%s", (features & CHIP_FEATURE_BT) ? " BT" : "");
    printf("%s", (features & CHIP_FEATURE_BLE) ? " BLE" : "");
    printf("%s", (features & CHIP_FEATURE_EMB_FLASH) ? " embedded-flash" : "");
    printf("%s", (features & CHIP_FEATURE_EMB_PSRAM) ? " embedded-psram" : "");
    printf("\n");
}

static void report_chip(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    printf("--- Chip ---\n");
    printf("IDF target      : %s\n", CONFIG_IDF_TARGET);
    printf("IDF version     : %s\n", esp_get_idf_version());
    printf("CPU cores       : %d\n", chip_info.cores);
    printf("Silicon revision: v%d.%d\n", chip_info.revision / 100, chip_info.revision % 100);
    printf("Wi-Fi STA MAC   : %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    print_chip_features(chip_info.features);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash size      : %" PRIu32 " MB\n", flash_size / (1024U * 1024U));
    } else {
        ESP_LOGW(TAG, "failed to read flash size");
    }

    if (esp_psram_is_initialized()) {
        printf("PSRAM           : initialized, %u bytes total, %u bytes free\n",
               (unsigned)esp_psram_get_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        printf("PSRAM           : NOT initialized (check CONFIG_SPIRAM*)\n");
    }
    printf("Internal heap   : %u bytes free\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void report_bsp_pins(void)
{
    printf("\n--- BSP capabilities and pins ---\n");
    printf("Display   : %s, %dx%d\n", BSP_CAPS_DISPLAY ? "yes" : "no", BSP_LCD_H_RES, BSP_LCD_V_RES);
    printf("Touch     : %s\n", BSP_CAPS_TOUCH ? "yes" : "no");
    printf("Speaker   : %s\n", BSP_CAPS_AUDIO_SPEAKER ? "yes" : "no");
    printf("Microphone: %s\n", BSP_CAPS_AUDIO_MIC ? "yes" : "no");
    printf("SD card   : %s\n", BSP_CAPS_SDCARD ? "yes" : "no");
    printf("I2C       : SDA=%d SCL=%d\n", BSP_I2C_SDA, BSP_I2C_SCL);
    printf("SDMMC     : CMD=%d CLK=%d D0=%d\n", BSP_SD_CMD, BSP_SD_CLK, BSP_SD_D0);
}

/* Reads one register from a 7-bit I2C device. Returns ESP_OK and fills *out on success. */
static esp_err_t read_reg8(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, uint8_t *out)
{
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(dev, &reg, 1, out, 1, 200);
    i2c_master_bus_rm_device(dev);
    return err;
}

/* Scans 0x08..0x77, prints an i2cdetect-style grid, and returns a bitmap of hits. */
static void scan_i2c_bus(i2c_master_bus_handle_t bus, bool found[128])
{
    printf("\n--- I2C scan (SDA=%d SCL=%d) ---\n", BSP_I2C_SDA, BSP_I2C_SCL);
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int base = 0; base < 128; base += 16) {
        printf("%02x: ", base);
        for (int offset = 0; offset < 16; offset++) {
            const uint8_t address = base + offset;
            if (address < 0x08 || address > 0x77) {
                printf("   ");
                continue;
            }
            esp_err_t ret = i2c_master_probe(bus, address, I2C_PROBE_TIMEOUT_MS);
            if (ret == ESP_OK) {
                found[address] = true;
                printf("%02x ", address);
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\n");
    }
}

static void report_devices(i2c_master_bus_handle_t bus, const bool found[128])
{
    printf("\n--- Devices identified ---\n");
    int hits = 0;
    for (int addr = 0; addr < 128; addr++) {
        if (found[addr]) {
            hits++;
            printf("0x%02x  %s\n", addr, device_name(addr));
        }
    }
    if (hits == 0) {
        printf("(none — bus wiring or BSP init problem)\n");
    }

    /* Confirm the IMU by its WHO_AM_I rather than trusting the address alone. */
    if (found[ADDR_QMI8658]) {
        uint8_t who = 0;
        if (read_reg8(bus, ADDR_QMI8658, 0x00, &who) == ESP_OK) {
            printf("QMI8658 WHO_AM_I = 0x%02x (%s)\n", who, who == 0x05 ? "expected 0x05, OK" : "UNEXPECTED");
        } else {
            printf("QMI8658 WHO_AM_I read failed\n");
        }
    }

    /* AXP2101 chip ID lives at register 0x03. */
    if (found[ADDR_AXP2101]) {
        uint8_t id = 0;
        if (read_reg8(bus, ADDR_AXP2101, 0x03, &id) == ESP_OK) {
            printf("AXP2101 CHIP_ID  = 0x%02x\n", id);
        } else {
            printf("AXP2101 CHIP_ID read failed\n");
        }
    }

    printf("\n--- Board revision ---\n");
    if (found[ADDR_TOUCH_FT3168] && !found[ADDR_TOUCH_CST820]) {
        printf("V1: FT3168 touch @ 0x38  =>  SH8601 display controller\n");
    } else if (found[ADDR_TOUCH_CST820] && !found[ADDR_TOUCH_FT3168]) {
        printf("V2: CST820 touch @ 0x15  =>  CO5300 display controller\n");
    } else if (found[ADDR_TOUCH_FT3168] && found[ADDR_TOUCH_CST820]) {
        printf("AMBIGUOUS: both 0x15 and 0x38 responded — inspect manually\n");
    } else {
        printf("UNKNOWN: no touch controller answered at 0x15 or 0x38\n");
        printf("         (touch rail may be off until the display is initialised)\n");
    }
}

void app_main(void)
{
    printf("\n");
    printf("=================================================\n");
    printf(" ESP32-S3-Touch-AMOLED-1.8 — bringup-01\n");
    printf("=================================================\n");

    report_chip();
    report_bsp_pins();

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    bool found[128] = {false};
    scan_i2c_bus(bus, found);
    report_devices(bus, found);

    printf("\nbringup-01 complete.\n\n");

    while (true) {
        ESP_LOGI(TAG, "alive: internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
