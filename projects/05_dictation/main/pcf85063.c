#include "pcf85063.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static bool s_was_stopped, s_probed;

static const char *TAG = "rtc";

#define PCF85063_ADDR   0x51
#define REG_CONTROL_1   0x00
#define REG_CONTROL_2   0x01
#define REG_SECONDS     0x04 /* bit 7: OS, oscillator has stopped since last set */
#define OS_FLAG         0x80
#define COF_OFF         0x07 /* Control_2 COF[2:0] = 111: CLKOUT disabled */

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[16] = {reg};
    if (n > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(tmp + 1, buf, n);
    return i2c_master_transmit(s_dev, tmp, n + 1, 100);
}

esp_err_t pcf85063_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev), TAG, "add device");

    uint8_t c2 = 0;
    ESP_RETURN_ON_ERROR(rd(REG_CONTROL_2, &c2, 1), TAG, "read control_2");
    if ((c2 & 0x07) != COF_OFF) {
        c2 = (c2 & ~0x07) | COF_OFF;
        ESP_RETURN_ON_ERROR(wr(REG_CONTROL_2, &c2, 1), TAG, "write control_2");
        ESP_LOGI(TAG, "CLKOUT was on (control_2 %02x); switched off", c2);
    }
    uint8_t sec = 0;
    ESP_RETURN_ON_ERROR(rd(REG_SECONDS, &sec, 1), TAG, "read seconds");
    s_was_stopped = sec & OS_FLAG;
    s_probed = true;
    ESP_LOGI(TAG, "PCF85063 ready, oscillator %s", (sec & OS_FLAG) ? "STOPPED since last set - time not valid" : "running");
    return ESP_OK;
}

esp_err_t pcf85063_get(struct tm *out)
{
    uint8_t r[7];
    ESP_RETURN_ON_ERROR(rd(REG_SECONDS, r, sizeof(r)), TAG, "read time");
    if (r[0] & OS_FLAG) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd2bin(r[0] & 0x7F);
    out->tm_min = bcd2bin(r[1] & 0x7F);
    out->tm_hour = bcd2bin(r[2] & 0x3F);
    out->tm_mday = bcd2bin(r[3] & 0x3F);
    out->tm_wday = r[4] & 0x07;
    out->tm_mon = bcd2bin(r[5] & 0x1F) - 1;
    out->tm_year = bcd2bin(r[6]) + 100; /* chip years 00-99 = 2000-2099 */
    return ESP_OK;
}

esp_err_t pcf85063_set(const struct tm *t)
{
    uint8_t r[7] = {
        bin2bcd((uint8_t)t->tm_sec), /* writing seconds clears OS */
        bin2bcd((uint8_t)t->tm_min),
        bin2bcd((uint8_t)t->tm_hour),
        bin2bcd((uint8_t)t->tm_mday),
        (uint8_t)t->tm_wday,
        bin2bcd((uint8_t)(t->tm_mon + 1)),
        bin2bcd((uint8_t)(t->tm_year - 100)),
    };
    return wr(REG_SECONDS, r, sizeof(r));
}

bool pcf85063_was_stopped(void)
{
    return !s_probed || s_was_stopped; /* not answering counts as not trustworthy */
}
