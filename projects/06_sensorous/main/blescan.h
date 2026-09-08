#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * BLE as a sensor: a NimBLE observer, listening only. It never connects, never
 * pairs, and advertises nothing itself.
 *
 * A window's worth of advertisements is aggregated per device before anything is
 * written, because a single phone emits several adverts a second and a raw dump
 * would be mostly the same MAC over and over. What comes out is one row per
 * device per window, with the strongest RSSI of the window and how many adverts
 * backed it.
 *
 * Read the note in census.h about resolvable private addresses before treating
 * any of these MACs as a location marker. Most of them rotate.
 */

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;   /* 0 public, 1 random static, 2 RPA, 3 non-resolvable - see census.h */
    int8_t rssi_best;
    int8_t rssi_last;
    uint16_t adverts;    /* how many advertisements backed this row */
    char name[32];
    int32_t company;     /* manufacturer-data company ID, -1 if none */
    int8_t tx_power;     /* advertised TX power, or 127 when not present */
    bool connectable;
    bool is_new;         /* first time this run */
} blescan_dev_t;

typedef void (*blescan_dev_cb_t)(const blescan_dev_t *d, void *ctx);

/*
 * Bring up the controller and the NimBLE host, and wait for it to sync.
 * Costs internal RAM whether or not a window is running, which is why it is a
 * boot-time decision rather than something the scan cycle turns on and off.
 */
esp_err_t blescan_init(void);

/* True once the host has synced and a window can run. */
bool blescan_ready(void);

/*
 * Listen for `ms`, then report each distinct device once through `cb`.
 * Blocks for the window. Returns the number of distinct devices, or -1.
 * Do not call while a Wi-Fi sweep is running - they share the antenna.
 */
int blescan_window(int ms, blescan_dev_cb_t cb, void *ctx);

/* Totals for the run. */
uint32_t blescan_adverts(void);
uint32_t blescan_windows(void);
int blescan_last_devices(void);

/* Devices that did not fit the per-window table, for the run. */
uint32_t blescan_window_overflow(void);
