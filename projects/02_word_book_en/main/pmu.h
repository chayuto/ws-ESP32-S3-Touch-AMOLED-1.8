#pragma once

#include <stdbool.h>
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
 * CONFIG_WORDBOOK_POWER_LOG_S seconds and on every change. Returns true on a change.
 */
bool pmu_poll(void);

/* One power record now, tagged with `event`. */
void pmu_record(const char *event);
