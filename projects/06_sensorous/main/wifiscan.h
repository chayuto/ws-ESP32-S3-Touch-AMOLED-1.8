#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Wi-Fi as a sensor: sweep every channel and report the beacons heard. No
 * association, no traffic; passive by default, so the board is silent on the air.
 *
 * The BSSID is the field that matters. It is a globally unique address bolted to
 * a box that does not move, which is what makes a set of them a location fix -
 * Google's Geolocation API takes exactly {macAddress, signalStrength, channel}
 * and returns a lat/long. The SSID is for humans and is not sent anywhere.
 */

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t authmode;  /* wifi_auth_mode_t */
    bool is_new;       /* first time this run */
} wifiscan_ap_t;

typedef void (*wifiscan_ap_cb_t)(const wifiscan_ap_t *ap, void *ctx);

/* Allocate the result buffer in PSRAM. The radio is brought up by the caller. */
esp_err_t wifiscan_init(void);

/*
 * One full sweep, blocking (roughly channels x dwell, ~2 s at the defaults).
 * Every AP is fed to the census and then to `cb`. Returns how many were heard,
 * or -1 if the scan failed. Do not call while BLE is scanning.
 */
int wifiscan_run(wifiscan_ap_cb_t cb, void *ctx);

/* How long the last sweep took, ms. */
int wifiscan_last_ms(void);

/* Sweeps completed, and how many APs the last one heard. */
uint32_t wifiscan_cycles(void);
int wifiscan_last_count(void);

/* Human-readable authmode, for records. */
const char *wifiscan_auth_name(uint8_t authmode);
