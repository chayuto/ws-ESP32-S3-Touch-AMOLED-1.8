#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Setup mode: a Wi-Fi access point and a one-page web app where a parent drops
 * photos onto words. The browser does the cropping and pixel packing; the board
 * only writes bytes to the card. Book edits raise a flag the main loop reloads on.
 */
esp_err_t webui_start(void);
void webui_stop(void);

/* True once after the page changed the book (photo written, word added/removed). */
bool webui_take_book_changed(void);
