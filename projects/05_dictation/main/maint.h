#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Maintenance mode: the board joins the home Wi-Fi and serves a small read-only
 * HTTP API for metrics, the flight recorder, and the decode records. Entered by a
 * long press on BOOT or the serial command 'm'; left the same way.
 *
 * The radio exists ONLY for this. The recognition path never uses it and audio
 * never leaves the board in any mode - see docs/design/05_dictation.md. The
 * recogniser is stopped for the duration to make room in internal RAM.
 *
 * 02's book-management routes are deliberately absent: there is no book here.
 */

typedef struct {
    uint32_t utterances;      /* decodes seen since boot */
    uint32_t probe_results;   /* MX-1: MultiNet results examined */
    uint32_t probe_raw;       /* MX-1: results with a non-empty raw_string */
    uint32_t probe_differs;   /* MX-1: raw_string differing from the snapped string */
    const char *last_text;
    float last_prob;
    uint32_t loop_max_ms;
    uint32_t loop_turns;
} maint_app_state_t;

typedef void (*maint_state_cb_t)(maint_app_state_t *out);

esp_err_t maint_start(maint_state_cb_t cb);
void maint_stop(void);
bool maint_active(void);

/* Seconds since the last HTTP request. */
int maint_idle_s(void);

/* A request asked for a reboot; the app decides when. */
bool maint_take_reboot(void);

/* The IP address while active, for the screen. */
const char *maint_ip(void);
