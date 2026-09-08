#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Maintenance mode: the board joins the home Wi-Fi and serves the card over
 * HTTP. This is how the data gets off the board without pulling the microSD.
 *
 * Entered by a long press on BOOT or the serial command 'm'; left the same way.
 * While it is up, scanning stops - the radio cannot sweep channels and hold an
 * association at the same time, and a scan during a download would stall it.
 * That is a deliberate trade, and the mode says so on the screen.
 *
 * Read-only except for three POSTs that a person has to ask for: eject, mount
 * and reboot. Nothing here uploads to the board, and nothing leaves it except
 * files the operator asks for by name.
 */

typedef struct {
    const char *mode;         /* what the app calls the mode it is in */
    uint32_t scans;
    uint32_t wifi_known, ble_known;
    int wifi_last, ble_last;
    uint32_t records_sensor, records_radio, records_event;
    uint32_t loop_max_ms;
    uint32_t loop_turns;
} maint_app_state_t;

typedef void (*maint_state_cb_t)(maint_app_state_t *out);

/*
 * The app owns the card, so it - not the HTTP handler - performs an eject or a
 * remount when one is requested. These say a request arrived; the app acts on it
 * in its own loop, closes its files first, and the answer is visible in the next
 * /api/metrics.
 */
typedef enum { MAINT_REQ_NONE = 0, MAINT_REQ_EJECT, MAINT_REQ_MOUNT, MAINT_REQ_CENSUS, MAINT_REQ_REBOOT } maint_req_t;

esp_err_t maint_start(maint_state_cb_t cb);
void maint_stop(void);
bool maint_active(void);

/* Seconds since the last HTTP request. */
int maint_idle_s(void);

/* The next pending request from a client, or MAINT_REQ_NONE. Clears it. */
maint_req_t maint_take_request(void);

/* The IP address while active, for the screen. */
const char *maint_ip(void);
