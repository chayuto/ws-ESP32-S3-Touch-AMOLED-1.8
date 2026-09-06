#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/*
 * The screen is the entire product here: there are no photos and no sound, only
 * text. Three regions, top to bottom - a state chip, the scrolling transcript,
 * and a detail line.
 *
 * Panel control below is carried over from 02_word_book_en/cards.c unchanged,
 * including the reason it looks like this. Do not simplify it back.
 */

/* What the board is doing, shown as a chip. A screen that shows nothing is
 * indistinguishable from a dead one - 02 paid for that lesson twice. */
typedef enum {
    DISPLAY_IDLE = 0,   /* listening, nothing heard yet */
    DISPLAY_HEARING,    /* VAD says someone is speaking */
    DISPLAY_WORKING,    /* utterance closed, recogniser running */
    DISPLAY_ERROR,      /* something is wrong and is named on the detail line */
} display_state_t;

/*
 * Bring up the panel, touch and LVGL with a draw buffer in internal, DMA-capable
 * RAM. bsp_display_start() puts the buffer wherever MALLOC_CAP_DEFAULT lands it,
 * which with PSRAM enabled means PSRAM and a bounce buffer per flush; under RAM
 * pressure that allocation fails and the screen freezes. Returns the display.
 */
lv_display_t *display_start(void);

/* Build the screen. Takes the display lock itself. */
void display_init(void);

/* Set the state chip. Takes the lock. */
void display_set_state(display_state_t st);

/* Append one decoded utterance to the transcript and scroll to it. */
void display_append(const char *text, float confidence);

/* Replace the detail line under the transcript. */
void display_detail(const char *text);

/* A full-screen message, for maintenance mode and fatal states. */
void display_message(const char *title, const char *detail);

/* Clear the transcript. */
void display_clear(void);

/* Diagnostics: force full brightness and redraw; re-run the panel init sequence. */
void display_force_bright(void);
void display_panel_reinit(void);

/*
 * Brightness 0-100, and a real display off. Use these, not bsp_display_brightness_set():
 * the BSP discards the QSPI transmit result and always reports ESP_OK.
 */
esp_err_t display_set_brightness(int pct);
esp_err_t display_off(void);
