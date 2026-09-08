#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every radio address the board has ever heard, this run, with the evidence
 * needed to place it: MAC, RSSI, when, how often. Two tables, one shape.
 *
 * Why a table and not just a line per sighting: a stationary board in a suburb
 * sees the same 30 access points every minute for a week. The per-scan file
 * (radio.jsonl) keeps the sightings because RSSI over time is the signal; the
 * census keeps one row per address so "what is around here" is a single small
 * file, and so the screen can say a true number.
 *
 * Both tables live in PSRAM and are guarded by a mutex - Wi-Fi results arrive on
 * the scan task, BLE adverts on the NimBLE host task, and the HTTP server reads
 * the lot from a third.
 *
 * ---------------------------------------------------------------------------
 * On BLE addresses, before anyone builds on them: most phones, watches and
 * earbuds advertise a Resolvable Private Address that rotates every ~15 minutes.
 * Those rows are noise for location - they will never be seen again. The rows
 * worth anything are `addr_type` 0 (public) and 1 (random static): beacons,
 * some fitness sensors, some fixed equipment. `addr_type` is recorded on every
 * row precisely so analysis can throw the rest away. Wi-Fi BSSIDs do not rotate
 * and are the real location signal here.
 * ---------------------------------------------------------------------------
 */

typedef struct {
    uint8_t mac[6];
    int8_t rssi_last;
    int8_t rssi_best;      /* strongest ever heard: the closest approach */
    uint32_t sightings;
    int64_t first_s, last_s;       /* wall clock, unix seconds; 0 if the clock was unset */
    uint32_t first_up_ms, last_up_ms; /* uptime, always meaningful */

    /* Wi-Fi only */
    char ssid[33];
    uint8_t channel;
    uint8_t authmode;      /* wifi_auth_mode_t */
    bool hidden;           /* beacon carried no SSID */

    /* BLE only */
    uint8_t addr_type;     /* 0 public, 1 random static, 2 RPA, 3 non-resolvable */
    char name[32];
    int32_t company;       /* manufacturer-data company ID, -1 if none */
    bool connectable;
} census_row_t;

typedef enum { CENSUS_WIFI = 0, CENSUS_BLE = 1 } census_kind_t;

/* Allocate both tables in PSRAM. Returns false if PSRAM could not supply them. */
bool census_init(void);

/*
 * Record one sighting. Returns true when the address had never been heard before.
 * `ssid` may be NULL or empty for a hidden AP; `name` may be NULL.
 */
bool census_wifi_seen(const uint8_t mac[6], const char *ssid, uint8_t channel, int8_t rssi, uint8_t authmode);
bool census_ble_seen(const uint8_t mac[6], uint8_t addr_type, const char *name, int32_t company, int8_t rssi,
                     bool connectable);

size_t census_count(census_kind_t kind);
size_t census_capacity(census_kind_t kind);

/* Addresses dropped because the table was full, for the run. */
uint32_t census_overflow(census_kind_t kind);

/* Total sightings fed in, whether or not they were new. */
uint32_t census_sightings(census_kind_t kind);

/*
 * Copy row `i` out under the lock. False when `i` is past the end. Copying
 * rather than handing back a pointer is deliberate: the HTTP server iterates
 * while the radios keep writing.
 */
bool census_row(census_kind_t kind, size_t i, census_row_t *out);

/* "aa:bb:cc:dd:ee:ff" into buf (18 bytes). */
const char *census_mac_str(const uint8_t mac[6], char *buf, size_t n);

/* BLE address type as a word, for records a human will read. */
const char *census_addr_type_name(uint8_t addr_type);

/* Forget everything. Used when the operator starts a new survey. */
void census_reset(void);
