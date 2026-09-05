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

/*
 * Second channel: a machine-readable file beside the log (JSON Lines), written by
 * sdlog_aux_write() from any task, drained and synced by the same task as the log.
 * Opened and closed together with the main file.
 */
esp_err_t sdlog_aux_open(const char *path);
void sdlog_aux_close(void);
void sdlog_aux_write(const char *line); /* one record, no trailing newline needed */
bool sdlog_aux_active(void);
const char *sdlog_aux_path(void);
long sdlog_aux_size(void);
esp_err_t sdlog_aux_truncate(void);

/* Current file's path and size in bytes (0 if none). */
const char *sdlog_path(void);
long sdlog_size(void);

/* Empty the current log file and start it again with a fresh boot header. */
esp_err_t sdlog_truncate(void);
