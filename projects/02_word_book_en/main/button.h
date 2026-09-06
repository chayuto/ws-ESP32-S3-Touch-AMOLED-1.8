#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The BOOT button on GPIO 0. Active low, hardware pull-up, debounced in software. */
void button_init(void);

typedef enum { BUTTON_NONE = 0, BUTTON_SHORT, BUTTON_LONG } button_event_t;

/*
 * Poll from the main loop. SHORT is reported on release of a press under the
 * long threshold; LONG fires once while still held, at the threshold.
 */
button_event_t button_poll(void);

/* How long the press behind the last event was held, in ms (LONG: the threshold). */
uint32_t button_last_held_ms(void);
