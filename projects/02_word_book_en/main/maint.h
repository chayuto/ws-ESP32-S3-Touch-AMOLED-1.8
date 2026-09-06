#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Maintenance mode: the board joins the home Wi-Fi as a client and serves a
 * small HTTP API plus one page for managing the card - photos, words, the
 * log - and reading device metrics. The recogniser is stopped for the
 * duration to make room. Entered by a long press on BOOT or the serial
 * command 'm'; left the same way, or after CONFIG_WORDBOOK_MAINT_IDLE_MIN
 * minutes without a request.
 */

typedef struct {
    uint32_t heard;
    const char *last_word;
    float last_prob;
    int words;
    bool files_ok;
    uint32_t loop_max_ms;  /* longest main-loop turn since the last state record */
    uint32_t loop_turns;
    char stack_json[160];  /* {"main":bytes,...} headroom per task, -1 when not running */
} maint_app_state_t;

/* The app fills this on request so metrics reflect the live state. */
typedef void (*maint_state_cb_t)(maint_app_state_t *out);

esp_err_t maint_start(maint_state_cb_t cb);
void maint_stop(void);
bool maint_active(void);

/* Seconds since the last HTTP request. */
int maint_idle_s(void);

/* A request changed the book (upload, delete). Cleared when read. */
bool maint_take_book_changed(void);

/* A request asked for a reboot; the app decides when. */
bool maint_take_reboot(void);

/* The IP address while active, for the screen. */
const char *maint_ip(void);
