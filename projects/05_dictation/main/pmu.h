#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * AXP2101 power-management chip at I2C 0x34. It owns every rail on the board,
 * charges the battery and knows whether USB is present. Register map from the
 * vendor's XPowersLib (ref/demo/.../90_axp2101_pmu); datasheet in ref/datasheets.
 */

typedef struct {
    bool vbus_in;        /* USB power present and good */
    bool batt_present;
    bool charging;
    uint8_t chg_state;   /* 0 tri, 1 pre, 2 CC, 3 CV, 4 done, 5 not charging */
    uint16_t vbat_mv, vbus_mv, vsys_mv;
    uint8_t batt_pct;
} pmu_status_t;

/* Probe the chip, enable the ADCs, bring ALDO1/ALDO2 to 3.3 V as the vendor does, dump every rail. */
esp_err_t pmu_init(void);

esp_err_t pmu_read(pmu_status_t *out);

/* Log every rail's enable bit and voltage. Cheap; called at boot and on any VBUS change. */
void pmu_dump_rails(const char *why);

/*
 * Call from the main loop. Logs and re-dumps the rails when VBUS appears or
 * disappears, and writes a power record (JSON Lines, sdlog aux channel 1) every
 * CONFIG_DICT_POWER_LOG_S seconds and on every change. Returns true on a change.
 */
bool pmu_poll(void);

/*
 * The app composes the periodic state record (it knows the screen, the heap, the
 * recogniser). pmu_poll() calls this every CONFIG_DICT_POWER_LOG_S seconds and
 * on every VBUS change, with `event` = "periodic" | "usb_in" | "usb_out".
 */
typedef void (*pmu_record_cb_t)(const char *event);
void pmu_set_record_cb(pmu_record_cb_t cb);

/* Rail enable registers, for the record. */
void pmu_rail_bits(uint8_t *dcdc_en, uint8_t *ldo_en0, uint8_t *ldo_en1);

/* Seconds between periodic records; the app slows it while asleep. */
void pmu_set_record_period_s(int seconds);

/* The PMU's own die temperature (REG 3C/3D), degrees C; NAN if it did not answer. Two register reads. */
float pmu_die_temp_c(void);

/*
 * Charger controls, for the thermal guard. Charging is the biggest heater on the board.
 * Enable/disable is REG 18 bit 1; it persists in the PMU until the PMU loses power, so
 * pmu_init() turns charging back on at every boot and says so if it found it off.
 */
esp_err_t pmu_set_charging(bool on);
bool pmu_charging_enabled(void);

/* Charger configuration as found: input limit, charge current and target, the PMU's own thermal throttle. */
int pmu_input_limit_ma(void);
int pmu_charge_ma(void);
int pmu_charge_target_mv(void);
int pmu_thermal_reg_c(void);
void pmu_charger_text(char *buf, size_t n);

/*
 * Read-only extras for the records. IRQ status REG 48/49/4A, three bytes, read and never
 * cleared, so they latch everything since the PMU last lost power: PWR short/long press
 * (49 bits 3/2), VBUS in/out (49 bits 7/6), charge start/done (4A bits 3/4), gauge
 * warning levels (48 bits 7/6), die over-temperature (4A bit 2), safety timer (4A bit 1).
 */
void pmu_irq_status(uint8_t out[3]);

/* The TS pin ADC, raw (REG 36/37); tells whether a battery thermistor is there. -1 on error. */
int pmu_ts_raw(void);

/* Gauge low-battery warning levels, percent (REG 1A): level 1 and level 2. */
void pmu_low_batt_levels(int *lvl1_pct, int *lvl2_pct);

/* Soft power-off (REG 10 bit 0): every rail off, the PMU waits for PWR or a USB insert. Does not return on success. */
esp_err_t pmu_power_off(void);

/*
 * Why the PMU last powered on (REG 20) and off (REG 21): raw bits and readable names
 * ("vbus_insert", "pwr_key_held", ... or "none"). Answers "did it lose power?" at boot.
 */
void pmu_power_sources(uint8_t *on_bits, uint8_t *off_bits, char *on_txt, size_t on_n, char *off_txt, size_t off_n);
