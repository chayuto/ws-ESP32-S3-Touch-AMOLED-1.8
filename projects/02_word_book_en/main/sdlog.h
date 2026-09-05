#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Mirror every ESP_LOG line to a file on the SD card, append mode, for
 * analysis afterwards. Lines are captured from the first call onward into a
 * PSRAM ring; a low-priority task drains the ring to the file whenever one
 * is open and syncs it every two seconds. Console output is unchanged.
 */

/* Install the log hook and start the drain task. Call first thing in app_main. */
void sdlog_init(void);

/* A card is available: open (append) `path` and write a boot header. */
esp_err_t sdlog_open(const char *path);

/* The card went away (or is about to): close the file. Logging continues into the ring. */
void sdlog_close(void);

/* True while a log file is open on the card. */
bool sdlog_active(void);
