#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

/*
 * PCF85063 real-time clock at I2C 0x51, on the BSP's shared bus. Keeps time
 * across power cycles. Also owns the 32 kHz CLKOUT pin, which we switch off:
 * nothing uses it and a square wave on the board is not a friend of the mic.
 */
esp_err_t pcf85063_init(void);

/* True if the oscillator-stopped flag was set at init: the RTC had lost its time. */
bool pcf85063_was_stopped(void);

/* Read the clock. Fails if the oscillator-stopped flag is set (time is not trustworthy). */
esp_err_t pcf85063_get(struct tm *out);

/* Write the clock and clear the oscillator-stopped flag. UTC. */
esp_err_t pcf85063_set(const struct tm *t);
