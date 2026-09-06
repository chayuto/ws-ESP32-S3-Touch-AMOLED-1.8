#include "pmu.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "pmu";

#define AXP2101_ADDR      0x34
#define REG_STATUS1       0x00 /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2       0x01 /* bits7:5 = 001 charging; bit3 = 1 means no VBUS; bits2:0 charge state */
#define REG_COMMON_CFG    0x10 /* bit0 soft power-off (RWAC), bit1 restart */
#define REG_IIN_LIMIT     0x16 /* bits2:0: 100/500/900/1000/1500/2000 mA */
#define REG_CHG_GAUGE_WDT 0x18 /* bit1 cell battery charge enable, bit3 gauge enable */
#define REG_LOW_BATT_WARN 0x1A /* bits7:4 level2 = 5 + n %, bits3:0 level1 = n % */
#define REG_ADC_TS        0x36 /* H6L8 */
#define REG_IRQ_STATUS0   0x48 /* 48/49/4A, RW1C; we only read */
#define REG_CHIP_ID       0x03 /* 0x4a */
#define REG_PWRON_STATUS  0x20 /* why the PMU last powered on; holds until the next power-on */
#define REG_PWROFF_STATUS 0x21 /* why it last powered off; holds until the PMU itself loses power */
#define REG_ADC_ENABLE    0x30 /* bit0 VBAT, bit2 VBUS, bit3 VSYS, bit4 die temp */
#define REG_ADC_VBAT      0x34 /* H5L8 */
#define REG_ADC_VBUS      0x38 /* H6L8 */
#define REG_ADC_VSYS      0x3A /* H6L8 */
#define REG_ADC_TDIE      0x3C /* H6L8; 22 + (7274 - raw) / 20 degC, the vendor library's formula */
#define REG_ICC           0x62 /* bits4:0: 25*N mA for N<=8, 200+100*(N-8) above */
#define REG_CV            0x64 /* bits2:0: 1=4.0 2=4.1 3=4.2 4=4.35 5=4.4 V */
#define REG_TREG          0x65 /* bits1:0: charger thermal regulation at 60/80/100/120 C die */
#define REG_DCDC_ENABLE   0x80 /* bit0..4 DCDC1..5 */
#define REG_DCDC1_VOL     0x82 /* 1500 + 100*n */
#define REG_LDO_ENABLE0   0x90 /* bit0 ALDO1 .. bit3 ALDO4, bit4 BLDO1, bit5 BLDO2, bit6 CPUSLDO, bit7 DLDO1 */
#define REG_LDO_ENABLE1   0x91 /* bit0 DLDO2 */
#define REG_ALDO1_VOL     0x92 /* 500 + 100*n, n<=0x1F; ALDO2..4 follow, then BLDO1/2, CPUSLDO (50 mV), DLDO1, DLDO2 (50 mV) */
#define REG_BATT_PCT      0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_ready, s_last_vbus;
static int64_t s_last_poll_us;
static int s_record_period_s = CONFIG_WORDBOOK_POWER_LOG_S;

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
    if (out->vbus_mv > 6000) {
        out->vbus_mv = rd16(REG_ADC_VBUS, 0x3F); /* 16371 mV right at plug-in: the ADC had not settled */
    }
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

    /* Charging is the biggest heater on the board. The thermal guard may have switched it off
     * before a power-off, and REG 18 survives that; it must not stay off for the next run. */
    uint8_t chg = 0;
    rd(REG_CHG_GAUGE_WDT, &chg);
    if (!(chg & 0x02)) {
        ESP_LOGW(TAG, "charger was OFF at boot (a thermal trip last run?); turning it back on");
        wr(REG_CHG_GAUGE_WDT, chg | 0x02);
    }
    /* The PMU's own thermal throttle: it lowers the charge current itself once its die passes
     * this line, firmware running or not. The chip default is 100 C; a toy should be gentler. */
    uint8_t treg = 0;
    rd(REG_TREG, &treg);
    uint8_t want = CONFIG_WORDBOOK_PMU_TREG_C >= 120 ? 3 : CONFIG_WORDBOOK_PMU_TREG_C >= 100 ? 2 : CONFIG_WORDBOOK_PMU_TREG_C >= 80 ? 1 : 0;
    if ((treg & 0x03) != want) {
        ESP_LOGI(TAG, "charger thermal regulation %d C -> %d C", 60 + 20 * (treg & 0x03), 60 + 20 * want);
        wr(REG_TREG, (treg & ~0x03) | want);
    }
    char chg_txt[120];
    pmu_charger_text(chg_txt, sizeof(chg_txt));
    ESP_LOGI(TAG, "charger: %s", chg_txt);

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

static pmu_record_cb_t s_record_cb;

void pmu_set_record_cb(pmu_record_cb_t cb)
{
    s_record_cb = cb;
}

void pmu_rail_bits(uint8_t *dcdc_en, uint8_t *ldo_en0, uint8_t *ldo_en1)
{
    uint8_t a = 0, b = 0, c = 0;
    if (s_ready) {
        rd(REG_DCDC_ENABLE, &a);
        rd(REG_LDO_ENABLE0, &b);
        rd(REG_LDO_ENABLE1, &c);
    }
    if (dcdc_en) *dcdc_en = a;
    if (ldo_en0) *ldo_en0 = b;
    if (ldo_en1) *ldo_en1 = c;
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
        if (s_record_cb) s_record_cb(st.vbus_in ? "usb_in" : "usb_out");
        last_record_us = esp_timer_get_time();
    } else if (esp_timer_get_time() - last_record_us >= (int64_t)s_record_period_s * 1000000) {
        if (s_record_cb) s_record_cb("periodic");
        last_record_us = esp_timer_get_time();
    }
    return changed;
}

void pmu_set_record_period_s(int seconds)
{
    s_record_period_s = seconds > 0 ? seconds : CONFIG_WORDBOOK_POWER_LOG_S;
}

static void bits_to_text(uint8_t bits, const char *const names[8], char *out, size_t n)
{
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < 8 && len < n; i++) {
        if (((bits >> i) & 1) && names[i]) {
            len += snprintf(out + len, n - len, "%s%s", len ? "+" : "", names[i]);
        }
    }
    if (out[0] == '\0') {
        snprintf(out, n, "none");
    }
}

void pmu_power_sources(uint8_t *on_bits, uint8_t *off_bits, char *on_txt, size_t on_n, char *off_txt, size_t off_n)
{
    /* REG 20 / REG 21 bit names, datasheet 6.13.2.18-19. */
    static const char *const on_names[8] = {"pwr_key", "irq_pin", "vbus_insert", "battery_charged", "battery_insert", "en_high", NULL, NULL};
    static const char *const off_names[8] = {"pwr_key_held", "software", "en_low", "vsys_undervoltage",
                                             "vbus_overvoltage", "dcdc_undervoltage", "dcdc_overvoltage", "die_overtemp"};
    uint8_t on = 0, off = 0;
    if (s_ready) {
        rd(REG_PWRON_STATUS, &on);
        rd(REG_PWROFF_STATUS, &off);
    }
    if (on_bits) *on_bits = on;
    if (off_bits) *off_bits = off;
    if (on_txt) bits_to_text(on, on_names, on_txt, on_n);
    if (off_txt) bits_to_text(off, off_names, off_txt, off_n);
}

float pmu_die_temp_c(void)
{
    uint8_t h = 0, l = 0;
    if (!s_ready || rd(REG_ADC_TDIE, &h) != ESP_OK || rd(REG_ADC_TDIE + 1, &l) != ESP_OK) {
        return NAN;
    }
    int raw = ((h & 0x3F) << 8) | l;
    return 22.0f + (7274 - raw) / 20.0f;
}

esp_err_t pmu_set_charging(bool on)
{
    uint8_t v = 0;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(rd(REG_CHG_GAUGE_WDT, &v), TAG, "reg 18");
    bool was = v & 0x02;
    if (was == on) {
        return ESP_OK;
    }
    esp_err_t err = wr(REG_CHG_GAUGE_WDT, on ? (v | 0x02) : (v & ~0x02));
    if (on) {
        ESP_LOGI(TAG, "charger on (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "charger OFF (%s)", esp_err_to_name(err));
    }
    return err;
}

bool pmu_charging_enabled(void)
{
    uint8_t v = 0;
    return s_ready && rd(REG_CHG_GAUGE_WDT, &v) == ESP_OK && (v & 0x02);
}

int pmu_input_limit_ma(void)
{
    static const int ma[8] = {100, 500, 900, 1000, 1500, 2000, -1, -1};
    uint8_t v = 0;
    return (s_ready && rd(REG_IIN_LIMIT, &v) == ESP_OK) ? ma[v & 0x07] : -1;
}

int pmu_charge_ma(void)
{
    uint8_t v = 0;
    if (!s_ready || rd(REG_ICC, &v) != ESP_OK) {
        return -1;
    }
    int n = v & 0x1F;
    return n <= 8 ? 25 * n : 200 + 100 * (n - 8);
}

int pmu_charge_target_mv(void)
{
    static const int mv[8] = {-1, 4000, 4100, 4200, 4350, 4400, -1, -1};
    uint8_t v = 0;
    return (s_ready && rd(REG_CV, &v) == ESP_OK) ? mv[v & 0x07] : -1;
}

int pmu_thermal_reg_c(void)
{
    uint8_t v = 0;
    return (s_ready && rd(REG_TREG, &v) == ESP_OK) ? 60 + 20 * (v & 0x03) : -1;
}

void pmu_charger_text(char *buf, size_t n)
{
    snprintf(buf, n, "input limit %d mA, charge %d mA to %d mV, PMU throttles charging above %d C die, charger %s",
             pmu_input_limit_ma(), pmu_charge_ma(), pmu_charge_target_mv(), pmu_thermal_reg_c(),
             pmu_charging_enabled() ? "on" : "OFF");
}

esp_err_t pmu_power_off(void)
{
    uint8_t v = 0;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(rd(REG_COMMON_CFG, &v), TAG, "reg 10");
    ESP_LOGE(TAG, "soft power-off: every rail goes down now; PWR or a USB insert brings the board back");
    ESP_RETURN_ON_ERROR(wr(REG_COMMON_CFG, v | 0x01), TAG, "reg 10");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* the rails should be gone before this returns */
    ESP_LOGE(TAG, "still running 2 s after the power-off write");
    return ESP_FAIL;
}

void pmu_irq_status(uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (s_ready) {
        rd(REG_IRQ_STATUS0, &out[0]);
        rd(REG_IRQ_STATUS0 + 1, &out[1]);
        rd(REG_IRQ_STATUS0 + 2, &out[2]);
    }
}

int pmu_ts_raw(void)
{
    uint8_t h = 0, l = 0;
    if (!s_ready || rd(REG_ADC_TS, &h) != ESP_OK || rd(REG_ADC_TS + 1, &l) != ESP_OK) {
        return -1;
    }
    return ((h & 0x3F) << 8) | l;
}

void pmu_low_batt_levels(int *lvl1_pct, int *lvl2_pct)
{
    uint8_t v = 0;
    if (s_ready) {
        rd(REG_LOW_BATT_WARN, &v);
    }
    if (lvl1_pct) *lvl1_pct = v & 0x0F;
    if (lvl2_pct) *lvl2_pct = 5 + (v >> 4);
}
