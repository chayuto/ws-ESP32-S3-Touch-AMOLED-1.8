#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The card can be absent at boot, inserted later, or yanked mid-use. There is
 * no card-detect line on this board, so presence is polled: a mount attempt
 * when absent, a status command when present. Both are cheap (~50 ms / ~1 ms).
 */

/* First attempt, at boot. Logs the result at full volume. */
bool sdcard_init(void);

/* Call from the main loop; rate-limits itself. Returns true if presence changed. */
bool sdcard_poll(void);

bool sdcard_present(void);

/* A file operation failed. Re-check the card now instead of at the next poll. */
void sdcard_report_io_error(void);

/* How often that was reported, for the run. */
uint32_t sdcard_io_errors(void);

/* Size and free space as of the last read; 0 without a card. */
void sdcard_space(uint32_t *total_mb, uint32_t *free_mb);

/* Re-read free space. Costs a FAT walk (tens to hundreds of ms) - call it rarely. */
void sdcard_refresh_space(void);

/*
 * Deliberate eject. Unmounts the card and STOPS the poll from remounting it, so
 * the operator can pull it without racing a write. There is no card-detect pin
 * on this board and no mechanical interlock, so this is the only way to remove a
 * card safely while the board runs.
 *
 * The caller closes its files first - app_main does, through sdlog_close_all().
 */
void sdcard_eject(void);

/* Undo an eject and mount whatever is in the slot now. */
bool sdcard_remount(void);

/* True while ejected: no card is mounted and none will be until sdcard_remount(). */
bool sdcard_ejected(void);

/* How many times a card has been mounted this run. A reinsert increments it. */
uint32_t sdcard_mounts(void);
