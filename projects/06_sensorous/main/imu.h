#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * QMI8658 6-axis IMU at I2C 0x6B, on the BSP's shared bus. WHO_AM_I was read on
 * this board on 2026-09-05, but no reading was ever taken - this module is where
 * that changes, so treat the numbers as provisional until a record backs them.
 *
 * The IMU is sampled continuously by its own task at CONFIG_SENSOROUS_IMU_HZ and
 * summarised into the sensor record. A single instantaneous sample every 10
 * seconds would say nothing: a knock, a step, a door slam all live between the
 * samples. What is recorded is the window's mean, its peak and its RMS.
 */

typedef struct {
    bool valid;             /* the window contained at least one good read */
    uint32_t samples;
    uint32_t read_errors;

    float ax, ay, az;       /* mean acceleration, m/s^2, gravity included */
    float a_mag_mean;       /* mean |a| - about 9.81 at rest, and a check on the axes */
    float a_dyn_rms;        /* RMS of (|a| - 1 g): how much it actually moved */
    float a_dyn_peak;       /* the worst single sample in the window */

    float gx, gy, gz;       /* mean angular rate, deg/s */
    float g_mag_peak;       /* peak |gyro|, deg/s */

    float pitch_deg;        /* from the mean gravity vector; +nose up */
    float roll_deg;
    float temp_c;           /* the IMU's own die temperature */
} imu_stats_t;

/* Probe WHO_AM_I, configure ranges and ODR, start the sampling task. */
esp_err_t imu_init(void);

/* False when the chip did not answer at boot; every other call then no-ops. */
bool imu_present(void);

/* WHO_AM_I and REVISION as read at init, for the boot record. */
void imu_ident(uint8_t *who_am_i, uint8_t *revision);

/* The window since the last take, then start a new one. */
void imu_take(imu_stats_t *out);

/* The window so far, without ending it. For the screen. */
void imu_peek(imu_stats_t *out);
