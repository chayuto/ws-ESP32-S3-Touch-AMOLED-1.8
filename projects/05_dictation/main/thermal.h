#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Thermal guard. Two die temperatures on the board are cheap to read every second:
 * the ESP32-S3's own sensor and the AXP2101's (the charger is the biggest heater on
 * the board). The hotter of the two is "the board". Levels, in die degrees, from Kconfig:
 *
 *   warm  CONFIG_DICT_THERMAL_WARM_C   the app dims the screen and says so
 *   hot   CONFIG_DICT_THERMAL_HOT_C    the app sleeps (screen off, recogniser stopped),
 *                                          stops charging, and BOOT no longer wakes it
 *   trip  CONFIG_DICT_THERMAL_TRIP_C   the app powers the board off through the PMU
 *
 * A level changes only after CONFIG_DICT_THERMAL_SAMPLES consecutive seconds agree,
 * and drops only CONFIG_DICT_THERMAL_HYST_C below the line it crossed.
 * This module measures and decides; app_main acts.
 */

typedef enum { THERMAL_OK = 0, THERMAL_WARM, THERMAL_HOT, THERMAL_TRIP } thermal_level_t;

typedef struct {
    float chip_c;   /* ESP32-S3 die; NAN if the sensor is not there */
    float pmu_c;    /* AXP2101 die; NAN if it did not answer */
    float board_c;  /* the hotter of the two, or the simulated value */
    thermal_level_t level;
    uint32_t hot_s;  /* seconds at hot or above in the current episode */
    bool simulated;  /* serial `t` is feeding a made-up temperature */
} thermal_status_t;

typedef struct {
    int64_t when;    /* time() at the trip; small numbers mean the clock was not set */
    uint32_t up_s;
    float chip_c, pmu_c;
    uint32_t count;  /* trips ever recorded on this board */
} thermal_trip_t;

/* Install the chip sensor, open NVS, read the last trip marker. Call after pmu_init(). */
void thermal_init(void);

/* Call every main-loop turn, in every mode. Samples once a second. True when the level changed. */
bool thermal_poll(thermal_status_t *out);

/* The last sample, without taking a new one. */
void thermal_status(thermal_status_t *out);

const char *thermal_level_name(thermal_level_t level);

/* Persist "we powered off for heat" so the next boot can say so. */
void thermal_mark_trip(const thermal_status_t *st);

/* The last trip ever recorded on this board, if any. */
bool thermal_last_trip(thermal_trip_t *out);

/* Serial `t`: off -> warm -> hot -> trip (dry run: logged, never powers off) -> off. */
void thermal_simulate_step(void);
