#pragma once

#include "book.h"

typedef void (*cards_tap_cb_t)(void);

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

/* One-line status at the bottom (self-test progress, card state). */
void cards_status(const char *text);
