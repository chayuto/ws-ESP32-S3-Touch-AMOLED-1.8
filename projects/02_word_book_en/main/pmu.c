#include "pmu.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sdlog.h"
#include "timesync.h"

static const char *TAG = "pmu";

#define AXP2101_ADDR      0x34
#define REG_STATUS1       0x00 /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2       0x01 /* bits7:5 = 001 charging; bit3 = 1 means no VBUS; bits2:0 charge state */
#define REG_CHIP_ID       0x03 /* 0x4a */
#define REG_ADC_ENABLE    0x30 /* bit0 VBAT, bit2 VBUS, bit3 VSYS, bit4 die temp */
#define REG_ADC_VBAT      0x34 /* H5L8 */
#define REG_ADC_VBUS      0x38 /* H6L8 */
#define REG_ADC_VSYS      0x3A /* H6L8 */
#define REG_DCDC_ENABLE   0x80 /* bit0..4 DCDC1..5 */
#define REG_DCDC1_VOL     0x82 /* 1500 + 100*n */
#define REG_LDO_ENABLE0   0x90 /* bit0 ALDO1 .. bit3 ALDO4, bit4 BLDO1, bit5 BLDO2, bit6 CPUSLDO, bit7 DLDO1 */
#define REG_LDO_ENABLE1   0x91 /* bit0 DLDO2 */
#define REG_ALDO1_VOL     0x92 /* 500 + 100*n, n<=0x1F; ALDO2..4 follow, then BLDO1/2, CPUSLDO (50 mV), DLDO1, DLDO2 (50 mV) */
#define REG_BATT_PCT      0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_ready, s_last_vbus;
static int64_t s_last_poll_us;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static uint16_t rd16(uint8_t reg, uint8_t hi_mask)
{
    uint8_t h = 0, l = 0;
    if (rd(reg, &h) != ESP_OK || rd(reg + 1, &l) != ESP_OK) {
        return 0;
    }
    return (uint16_t)(((h & hi_mask) << 8) | l);
}

static int ldo_mv(uint8_t idx)
{
    uint8_t v = 0;
    if (rd(REG_ALDO1_VOL + idx, &v) != ESP_OK) {
        return -1;
    }
    v &= 0x1F;
    return (idx == 6 || idx == 8) ? 500 + 50 * v : 500 + 100 * v; /* CPUSLDO and DLDO2 step 50 mV */
}

void pmu_dump_rails(const char *why)
{
    if (!s_ready) {
        return;
    }
    uint8_t dc = 0, ldo0 = 0, ldo1 = 0, dc1 = 0;
    rd(REG_DCDC_ENABLE, &dc);
    rd(REG_LDO_ENABLE0, &ldo0);
    rd(REG_LDO_ENABLE1, &ldo1);
    rd(REG_DCDC1_VOL, &dc1);
    static const char *const ldo_names[] = {"ALDO1", "ALDO2", "ALDO3", "ALDO4", "BLDO1", "BLDO2", "CPUSLDO", "DLDO1", "DLDO2"};
    char line[200];
    int n = snprintf(line, sizeof(line), "rails (%s): DCDC1 %s %d mV, DCDC2..5 %c%c%c%c;", why, (dc & 1) ? "on" : "OFF",
                     1500 + 100 * (dc1 & 0x1F), (dc & 2) ? '+' : '-', (dc & 4) ? '+' : '-', (dc & 8) ? '+' : '-',
                     (dc & 16) ? '+' : '-');
    for (int i = 0; i < 9; i++) {
        bool on = (i < 8) ? (ldo0 >> i) & 1 : ldo1 & 1;
        n += snprintf(line + n, sizeof(line) - n, " %s %s%d;", ldo_names[i], on ? "" : "OFF@", ldo_mv(i));
        if (n >= (int)sizeof(line) - 20) {
            break;
        }
    }
    ESP_LOGI(TAG, "%s", line);
}

esp_err_t pmu_read(pmu_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t s1 = 0, s2 = 0, pct = 0;
    ESP_RETURN_ON_ERROR(rd(REG_STATUS1, &s1), TAG, "status1");
    ESP_RETURN_ON_ERROR(rd(REG_STATUS2, &s2), TAG, "status2");
    rd(REG_BATT_PCT, &pct);
    out->vbus_in = ((s1 >> 5) & 1) && !((s2 >> 3) & 1);
    out->batt_present = (s1 >> 3) & 1;
    out->charging = ((s2 >> 5) & 0x7) == 1;
    out->chg_state = s2 & 0x7;
    out->vbat_mv = out->batt_present ? rd16(REG_ADC_VBAT, 0x1F) : 0;
    out->vbus_mv = out->vbus_in ? rd16(REG_ADC_VBUS, 0x3F) : 0;
    out->vsys_mv = rd16(REG_ADC_VSYS, 0x3F);
    out->batt_pct = pct & 0x7F;
    return ESP_OK;
}

esp_err_t pmu_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev), TAG, "add device");
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(rd(REG_CHIP_ID, &id), TAG, "chip id");
    if (id != 0x4a) {
        ESP_LOGW(TAG, "chip id %02x, expected 4a", id);
    }
    s_ready = true;

    /* ADCs for battery, USB and system voltage. */
    uint8_t adc = 0;
    rd(REG_ADC_ENABLE, &adc);
    wr(REG_ADC_ENABLE, adc | 0x1D);

    /* The vendor's sketches bring these two up at 3.3 V before anything else. Our
     * firmware never did, and the panel went dark on battery. */
    uint8_t ldo0 = 0;
    rd(REG_LDO_ENABLE0, &ldo0);
    bool a1 = ldo0 & 1, a2 = ldo0 & 2;
    wr(REG_ALDO1_VOL, 28); /* 3300 mV */
    wr((REG_ALDO1_VOL + 1), 28);
    wr(REG_LDO_ENABLE0, ldo0 | 0x03);
    ESP_LOGI(TAG, "ALDO1 was %s, ALDO2 was %s; both now 3.3 V on", a1 ? "on" : "OFF", a2 ? "on" : "OFF");

    pmu_status_t st;
    if (pmu_read(&st) == ESP_OK) {
        ESP_LOGI(TAG, "vbus %s (%u mV), battery %s (%u mV, %u%%), %s, vsys %u mV", st.vbus_in ? "in" : "OUT", st.vbus_mv,
                 st.batt_present ? "present" : "ABSENT", st.vbat_mv, st.batt_pct,
                 st.charging ? "charging" : "not charging", st.vsys_mv);
        s_last_vbus = st.vbus_in;
    }
    pmu_dump_rails("boot");
    return ESP_OK;
}

static const char *const chg_names[] = {"tri", "pre", "cc", "cv", "done", "idle", "?", "?"};

void pmu_record(const char *event)
{
    pmu_status_t st;
    if (pmu_read(&st) != ESP_OK) {
        return;
    }
    uint8_t dc = 0, ldo0 = 0, ldo1 = 0;
    rd(REG_DCDC_ENABLE, &dc);
    rd(REG_LDO_ENABLE0, &ldo0);
    rd(REG_LDO_ENABLE1, &ldo1);
    char now[32], buf[320];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"power\",\"time\":\"%s\",\"up_ms\":%lld,\"event\":\"%s\",\"usb\":%d,\"vbus_mv\":%u,"
             "\"battery\":%d,\"vbat_mv\":%u,\"pct\":%u,\"vsys_mv\":%u,\"charging\":%d,\"chg\":\"%s\","
             "\"dcdc_en\":\"%02x\",\"ldo_en\":\"%02x%02x\"}",
             timesync_now_str(now, sizeof(now)), (long long)(esp_timer_get_time() / 1000), event, st.vbus_in, st.vbus_mv,
             st.batt_present, st.vbat_mv, st.batt_pct, st.vsys_mv, st.charging, chg_names[st.chg_state & 7], dc, ldo0, ldo1);
    sdlog_aux_write(1, buf);
}

bool pmu_poll(void)
{
    static int64_t last_record_us;
    if (!s_ready || esp_timer_get_time() - s_last_poll_us < 1000000) {
        return false;
    }
    s_last_poll_us = esp_timer_get_time();
    pmu_status_t st;
    if (pmu_read(&st) != ESP_OK) {
        return false;
    }
    bool changed = st.vbus_in != s_last_vbus;
    if (changed) {
        s_last_vbus = st.vbus_in;
        ESP_LOGW(TAG, "VBUS %s: battery %u mV (%u%%), vsys %u mV, %s", st.vbus_in ? "connected" : "REMOVED", st.vbat_mv,
                 st.batt_pct, st.vsys_mv, st.charging ? "charging" : "not charging");
        pmu_dump_rails(st.vbus_in ? "usb in" : "on battery");
        pmu_record(st.vbus_in ? "usb_in" : "usb_out");
        last_record_us = esp_timer_get_time();
    } else if (esp_timer_get_time() - last_record_us >= (int64_t)CONFIG_WORDBOOK_POWER_LOG_S * 1000000) {
        pmu_record("periodic");
        last_record_us = esp_timer_get_time();
    }
    return changed;
}
