#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Everything the board writes to the card goes through here: the flight recorder
 * (every ESP_LOG line, mirrored) and four record channels (JSON Lines).
 *
 * Lines are captured into PSRAM rings from the first call onward and a
 * low-priority task drains them to the card whenever files are open. Nothing
 * blocks the caller, and the console is written only when a host is attached -
 * read the comment above hook() before changing that.
 *
 * Rotation and retention live here too, because this is the only module that
 * knows how big the files actually are without stat()ing the card.
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
 * Record channels: machine-readable files beside the log, one JSON object per
 * line, written from any task and drained by the same task as the log.
 */
#define SDLOG_AUX_CHANNELS 4
#define SDLOG_CH_SENSORS 0 /* periodic sensor records */
#define SDLOG_CH_RADIO   1 /* per-scan Wi-Fi and BLE sightings */
#define SDLOG_CH_POWER   2 /* PMU rails, battery, VBUS */
#define SDLOG_CH_EVENTS  3 /* mode changes, card in/out, thermal, ejects */

esp_err_t sdlog_aux_open(int ch, const char *path);
void sdlog_aux_close(int ch);
void sdlog_aux_write(int ch, const char *line); /* one record, no trailing newline needed */
bool sdlog_aux_active(int ch);
const char *sdlog_aux_path(int ch);
long sdlog_aux_size(int ch);
esp_err_t sdlog_aux_truncate(int ch);

/* Lines lost because a ring was full (log and aux rings together), for the run. */
size_t sdlog_dropped(void);

/* Current log file's path and size in bytes (0 if none). */
const char *sdlog_path(void);
long sdlog_size(void);

/* Empty the current log file and start it again with a fresh boot header. */
esp_err_t sdlog_truncate(void);

/* ------------------------------------------------------------------------- */
/* Log management                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Call from the main loop. Rate-limits itself. Two jobs:
 *   - any open file past its size cap is closed, rotated to <path>.1 (older
 *     generations shifting up to CONFIG_SENSOROUS_KEEP_FILES) and reopened;
 *   - below CONFIG_SENSOROUS_CARD_MIN_FREE_MB the oldest generation of the
 *     largest family is deleted, loudly.
 * Returns true when it rotated or deleted something.
 */
bool sdlog_maintain(void);

/* Rotations and retention deletions performed, for the run. */
void sdlog_maintenance_counts(uint32_t *rotations, uint32_t *deletions);

/*
 * Close every file, flushed and synced, so the card can be pulled. The rings
 * keep filling; nothing is lost until they wrap. Reopen with sdlog_open_all().
 */
void sdlog_close_all(void);

/*
 * Open the log and all four record channels under `dir` on the mounted card,
 * creating the directory if needed. Called at mount and after a reinsert.
 */
esp_err_t sdlog_open_all(const char *dir);

/* The directory the files live in, or "" before the first open. */
const char *sdlog_dir(void);
