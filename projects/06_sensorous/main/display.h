#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/*
 * One screen, always showing what the board is measuring right now.
 *
 * It exists because a logger with a dark screen is indistinguishable from a
 * logger that has crashed, and this repo has already paid for that twice (see
 * CLAUDE.md, 2026-09-06). Every number here is also in the records; the screen
 * is the version a person can read from across a room.
 *
 * The panel control below is carried from 02_word_book_en/cards.c by way of
 * 05_dictation/display.c, unchanged, including the reasons it looks like this.
 * Do not simplify it back.
 */

typedef struct {
    const char *mode;       /* "stationary" | "mobile" | "maintenance" | "ejected" */
    const char *clock;      /* "14:22:01", or "unset" before a sync */
    const char *note;       /* one line: what it is doing this second */

    bool card;
    bool card_ejected;
    uint32_t card_free_mb;
    uint32_t records;       /* records written this run, all channels */

    int batt_pct;
    bool vbus;
    bool charging;
    float board_c;

    uint32_t wifi_known, ble_known; /* distinct addresses this run */
    int wifi_last, ble_last;        /* heard in the most recent cycle, -1 if none yet */
    uint32_t scans;

    float imu_pitch, imu_roll, imu_dyn; /* degrees, degrees, m/s^2 RMS */
    bool imu_ok;

    float sound_dbfs;   /* ambient level, RMS, DC removed */
    bool sound_ok;

    const char *ip;         /* non-NULL in maintenance mode */
} display_status_t;

/*
 * Bring up the panel, touch and LVGL with a draw buffer in internal, DMA-capable
 * RAM. bsp_display_start() puts the buffer wherever MALLOC_CAP_DEFAULT lands it,
 * which with PSRAM enabled means PSRAM and a bounce buffer per flush; under RAM
 * pressure that allocation fails and the screen freezes. Returns the display.
 */
lv_display_t *display_start(void);

/* Build the screen. Takes the display lock itself. */
void display_init(void);

/* Redraw every field. Takes the lock. Cheap enough to call once a second. */
void display_update(const display_status_t *st);

/* A full-screen message over the top - maintenance, eject, fatal states. */
void display_message(const char *title, const char *detail);
void display_clear_message(void);

/*
 * The card button, bottom right. A single tap - there is deliberately no
 * confirm step. CLAUDE.md's rule against multi-press gestures came from BOOT,
 * where a second press is what someone does when the screen looks dead, but the
 * reasoning carries: a labelled button that needs two taps is a button that
 * looks broken on the first one. Ejecting costs nothing that has not already
 * been flushed, and MOUNT appears in its place immediately.
 *
 * The tap is latched here and collected by the main loop, which owns the card.
 */
bool display_take_card_tap(void);

/* Diagnostics: force full brightness and redraw; re-run the panel init sequence. */
void display_force_bright(void);
void display_panel_reinit(void);

/*
 * Brightness 0-100, and a real display off. Use these, not bsp_display_brightness_set():
 * the BSP discards the QSPI transmit result and always reports ESP_OK.
 */
esp_err_t display_set_brightness(int pct);
esp_err_t display_off(void);
