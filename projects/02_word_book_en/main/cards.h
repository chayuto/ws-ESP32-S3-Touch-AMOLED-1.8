#pragma once

#include <stdint.h>

#include "book.h"
#include "lvgl.h"

/* A press on the glass, with where it landed (panel pixels; -1,-1 if unknown). */
typedef void (*cards_tap_cb_t)(int16_t x, int16_t y);

/*
 * Bring up the panel, touch and LVGL with a draw buffer in internal, DMA-capable
 * RAM. bsp_display_start() puts the buffer wherever MALLOC_CAP_DEFAULT lands it,
 * which with PSRAM enabled means PSRAM and a bounce buffer per flush; under RAM
 * pressure that allocation fails and the screen freezes. Returns the display.
 */
lv_display_t *cards_display_start(void);

/* Build the screen. Call under the display lock. `on_tap` fires from the LVGL task. */
void cards_init(cards_tap_cb_t on_tap);

/* Show the idle card. Takes the display lock itself. */
void cards_show_idle(void);

/*
 * Show a word: its photo if the book has one, else a large text card.
 * `confidence` is 0..1 and is shown small; `nth` is the running count.
 * Takes the display lock itself. Photo load is from file into PSRAM.
 */
bool cards_show_word(const book_word_t *word, float confidence, unsigned nth);

/* A plain card: big title, smaller detail line. For maintenance mode and the like. */
void cards_show_info(const char *title, const char *detail);

/* Show an already-filled PHOTO_BYTES RGB565 buffer as a card, with a caption. */
void cards_show_buffer(const uint8_t *rgb565, const char *caption);

/* Diagnostics: force full brightness and redraw; re-run the panel init sequence. */
void cards_force_bright(void);
void cards_panel_reinit(void);

/*
 * Brightness 0-100, and a real display off. Use these, not bsp_display_brightness_set():
 * the BSP discards the QSPI transmit result and always reports ESP_OK.
 */
esp_err_t cards_set_brightness(int pct);
esp_err_t cards_display_off(void);

/* One-line status at the bottom (self-test progress, card state). */
void cards_status(const char *text);
