#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blescan.h"
#include "imu.h"
#include "sound.h"
#include "wifiscan.h"

/*
 * The records. Everything this project exists to produce comes out of here, as
 * JSON Lines on the card - one self-describing object per line, `t` says which
 * kind. Four files, four purposes:
 *
 *   sensors.jsonl   t=sensor    IMU window, sound level, temperatures, power, heap, card
 *   radio.jsonl     t=scan      one header per scan cycle
 *                   t=ap        one line per Wi-Fi AP heard in that cycle
 *                   t=ble       one line per BLE device heard in that cycle
 *                   t=census_*  a full dump of the run's address tables, on demand
 *   power.jsonl     t=power     written by pmu.c on its own schedule
 *   events.jsonl    t=boot      what the board is, once per start
 *                   t=event     mode changes, card in/out, ejects, thermal, buttons
 *
 * Why one line per sighting rather than one array per scan: a dense scan is 30
 * APs and 100 BLE devices, which as a single line is 15 KB - larger than the
 * ring that carries it, so a busy moment would drop the whole record instead of
 * one row of it. Small lines also survive a card pulled mid-write: the file ends
 * at a line boundary and everything before it parses.
 *
 * ---------------------------------------------------------------------------
 * The Wi-Fi line uses Google's Geolocation API field names verbatim -
 * `macAddress`, `signalStrength`, `channel`, `age` - so a host-side script can
 * group the `ap` lines by `seq` and POST them with no field mapping at all.
 * That is the whole location story on this board: there is no GNSS and no cell
 * modem, and BSSIDs are the only thing in range that does not move.
 * ---------------------------------------------------------------------------
 */

/*
 * The mode is what the board is doing, not what the card is doing. An ejected
 * card shows up in every record's `card` object, which is where a reader looks
 * for it anyway - making it a mode would have hidden it from the sensor records.
 */
typedef enum {
    MODE_STATIONARY = 0,
    MODE_MOBILE,
    MODE_MAINTENANCE,
} sensorous_mode_t;

const char *srec_mode_name(sensorous_mode_t m);

/* "2026-09-09T14:22:01Z", or "unset" before the clock is set. */
const char *srec_iso_time(char *buf, size_t n);

/* Once at boot: what this board is, what answered, and how the clock was set. */
void srec_boot(sensorous_mode_t mode);

/*
 * One periodic sensor record. `imu` and `snd` may be all-zero structs when the
 * chip is absent or the measurement failed; each becomes null in the record
 * rather than a plausible-looking zero.
 */
void srec_sensor(sensorous_mode_t mode, const imu_stats_t *imu, const sound_level_t *snd);

/* A scan cycle. Call open first, then a line per sighting, then close. */
void srec_scan_begin(uint32_t seq, sensorous_mode_t mode);
void srec_scan_ap(uint32_t seq, const wifiscan_ap_t *ap);
void srec_scan_ble(uint32_t seq, const blescan_dev_t *d);
void srec_scan_end(uint32_t seq, int wifi_n, int wifi_ms, int ble_n, int ble_ms);

/* Dump both census tables to radio.jsonl. Can be thousands of lines; it paces itself. */
void srec_census_dump(void);

/*
 * Anything worth explaining a gap in the other files with. `detail` is free text
 * and is escaped here. Kept deliberately cheap so it is never a reason not to
 * record something.
 */
void srec_event(const char *what, const char *detail);
void srec_eventf(const char *what, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Records written this run, per channel, for the state line and the HTTP API. */
uint32_t srec_count_sensor(void);
uint32_t srec_count_radio(void);
uint32_t srec_count_event(void);
