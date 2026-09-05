#pragma once

#include <stdbool.h>

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
