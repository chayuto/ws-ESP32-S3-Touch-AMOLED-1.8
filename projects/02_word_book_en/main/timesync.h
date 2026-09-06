#pragma once

#include <stdbool.h>

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

/*
 * How the clock was set at boot: `clock` is the source that won ("ntp", "rtc", "build",
 * "none"); `rtc_valid` false means the RTC's oscillator had stopped, so it held no time;
 * `rtc_drift_s` is how far the RTC was ahead (+) of NTP, valid only when `drift_known`;
 * `ntp_ms` is what the NTP path took, join included, 0 if it did not run.
 */
typedef struct {
    const char *clock;
    bool rtc_valid;
    bool drift_known;
    int rtc_drift_s;
    int ntp_ms;
} timesync_info_t;
void timesync_info(timesync_info_t *out);

/* "2026-09-05 14:32:10" or "unset" into buf. */
const char *timesync_now_str(char *buf, size_t len);
