#pragma once

#include "esp_err.h"

/*
 * Set the system clock from the best source available, in this order:
 *   1. NTP over the home Wi-Fi, if CONFIG_WORDBOOK_WIFI_SSID is set and the
 *      network answers within a few seconds (Wi-Fi is torn down afterwards)
 *   2. the PCF85063 RTC on the board
 *   3. the firmware's build time
 * A successful NTP sync is written back to the RTC. Call once at boot, before
 * the recogniser starts; it blocks for up to ~20 s only when Wi-Fi is configured.
 */
void timesync_at_boot(void);

/* "2026-09-05 14:32:10" or "unset" into buf. */
const char *timesync_now_str(char *buf, size_t len);
