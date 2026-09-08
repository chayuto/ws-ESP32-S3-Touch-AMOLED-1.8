#include "imu.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
/* qmi8658.h defines M_PI unconditionally; math.h got there first. Same value. */
#undef M_PI
#include "qmi8658.h"
#include "sdkconfig.h"

static const char *TAG = "imu";

#define ONE_G_MPS2 9.80665f

static qmi8658_dev_t s_dev;
static bool s_present;
static uint8_t s_who, s_rev;
static SemaphoreHandle_t s_lock;

/* The accumulator. Sums rather than a ring buffer: the window can be ten seconds
 * of 100 Hz samples and none of them is interesting on its own. */
typedef struct {
    uint32_t n;
    uint32_t errors;
    double sax, say, saz;
    double sgx, sgy, sgz;
    double s_amag;
    double s_dyn2;      /* sum of (|a| - g)^2, for the RMS */
    float dyn_peak;
    float gmag_peak;
    float temp_c;
} accum_t;

static accum_t s_acc;

/* The ODR enums are a fixed ladder; pick the lowest rung at or above the ask so
 * a configured 100 Hz never silently becomes 31 Hz. */
static qmi8658_accel_odr_t accel_odr_for(int hz, int *actual)
{
    struct { qmi8658_accel_odr_t e; int hz; } ladder[] = {
        {QMI8658_ACCEL_ODR_31_25HZ, 31}, {QMI8658_ACCEL_ODR_62_5HZ, 62}, {QMI8658_ACCEL_ODR_125HZ, 125},
        {QMI8658_ACCEL_ODR_250HZ, 250},  {QMI8658_ACCEL_ODR_500HZ, 500},
    };
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        if (ladder[i].hz >= hz) {
            *actual = ladder[i].hz;
            return ladder[i].e;
        }
    }
    *actual = 500;
    return QMI8658_ACCEL_ODR_500HZ;
}

static qmi8658_gyro_odr_t gyro_odr_for(int hz)
{
    if (hz <= 31) return QMI8658_GYRO_ODR_31_25HZ;
    if (hz <= 62) return QMI8658_GYRO_ODR_62_5HZ;
    if (hz <= 125) return QMI8658_GYRO_ODR_125HZ;
    if (hz <= 250) return QMI8658_GYRO_ODR_250HZ;
    return QMI8658_GYRO_ODR_500HZ;
}

static void snapshot(const accum_t *a, imu_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->samples = a->n;
    out->read_errors = a->errors;
    out->temp_c = a->temp_c;
    if (a->n == 0) {
        out->valid = false;
        return;
    }
    out->valid = true;
    out->ax = (float)(a->sax / a->n);
    out->ay = (float)(a->say / a->n);
    out->az = (float)(a->saz / a->n);
    out->gx = (float)(a->sgx / a->n);
    out->gy = (float)(a->sgy / a->n);
    out->gz = (float)(a->sgz / a->n);
    out->a_mag_mean = (float)(a->s_amag / a->n);
    out->a_dyn_rms = sqrtf((float)(a->s_dyn2 / a->n));
    out->a_dyn_peak = a->dyn_peak;
    out->g_mag_peak = a->gmag_peak;

    /* Orientation from the mean acceleration vector. Valid only when the board is
     * not accelerating; a_dyn_rms in the same record says whether to believe it. */
    float horiz = sqrtf(out->ax * out->ax + out->ay * out->ay);
    out->pitch_deg = atan2f(-out->ax, sqrtf(out->ay * out->ay + out->az * out->az)) * 180.0f / (float)M_PI;
    out->roll_deg = (horiz > 0.0f) ? atan2f(out->ay, out->az) * 180.0f / (float)M_PI : 0.0f;
}

static void imu_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_SENSOROUS_IMU_HZ);
    TickType_t last = xTaskGetTickCount();
    int temp_countdown = 0;

    while (true) {
        qmi8658_data_t d;
        if (qmi8658_read_sensor_data(&s_dev, &d) == ESP_OK) {
            float amag = sqrtf(d.accelX * d.accelX + d.accelY * d.accelY + d.accelZ * d.accelZ);
            float dyn = fabsf(amag - ONE_G_MPS2);
            float gmag = sqrtf(d.gyroX * d.gyroX + d.gyroY * d.gyroY + d.gyroZ * d.gyroZ);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_acc.n++;
            s_acc.sax += d.accelX;
            s_acc.say += d.accelY;
            s_acc.saz += d.accelZ;
            s_acc.sgx += d.gyroX;
            s_acc.sgy += d.gyroY;
            s_acc.sgz += d.gyroZ;
            s_acc.s_amag += amag;
            s_acc.s_dyn2 += (double)dyn * dyn;
            if (dyn > s_acc.dyn_peak) {
                s_acc.dyn_peak = dyn;
            }
            if (gmag > s_acc.gmag_peak) {
                s_acc.gmag_peak = gmag;
            }
            xSemaphoreGive(s_lock);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_acc.errors++;
            xSemaphoreGive(s_lock);
        }

        /* The die temperature moves in minutes, not milliseconds. Once a second
         * is plenty, and it keeps two extra register reads off the shared I2C
         * bus that the PMU, the RTC and the touch controller also want. */
        if (--temp_countdown <= 0) {
            float t = 0;
            if (qmi8658_read_temp(&s_dev, &t) == ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_acc.temp_c = t;
                xSemaphoreGive(s_lock);
            }
            temp_countdown = CONFIG_SENSOROUS_IMU_HZ;
        }

        vTaskDelayUntil(&last, period > 0 ? period : 1);
    }
}

esp_err_t imu_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = qmi8658_init(&s_dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not found at 0x%02x: %s - IMU fields will be absent from every record",
                 QMI8658_ADDRESS_HIGH, esp_err_to_name(err));
        return err;
    }
    if (qmi8658_get_who_am_i(&s_dev, &s_who) != ESP_OK || s_who != 0x05) {
        ESP_LOGE(TAG, "WHO_AM_I is 0x%02x, expected 0x05", s_who);
        return ESP_ERR_NOT_FOUND;
    }

    int actual_hz = 0;
    /* 8 g and 512 dps: a hand-carried board knocked against a table peaks well
     * above 2 g, and clipping is silent. Resolution is not the scarce thing here. */
    ESP_ERROR_CHECK(qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G));
    ESP_ERROR_CHECK(qmi8658_set_gyro_range(&s_dev, QMI8658_GYRO_RANGE_512DPS));
    ESP_ERROR_CHECK(qmi8658_set_accel_odr(&s_dev, accel_odr_for(CONFIG_SENSOROUS_IMU_HZ, &actual_hz)));
    ESP_ERROR_CHECK(qmi8658_set_gyro_odr(&s_dev, gyro_odr_for(CONFIG_SENSOROUS_IMU_HZ)));
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    qmi8658_set_gyro_unit_dps(&s_dev, true);
    ESP_ERROR_CHECK(qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO));

    s_present = true;
    ESP_LOGI(TAG, "QMI8658 up: WHO_AM_I 0x%02x, +-8 g, +-512 dps, sampling at %d Hz (chip ODR %d Hz)", s_who,
             CONFIG_SENSOROUS_IMU_HZ, actual_hz);

    xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 4, NULL, 1);
    return ESP_OK;
}

bool imu_present(void)
{
    return s_present;
}

void imu_ident(uint8_t *who_am_i, uint8_t *revision)
{
    if (who_am_i) *who_am_i = s_who;
    if (revision) *revision = s_rev;
}

void imu_take(imu_stats_t *out)
{
    if (!s_present) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot(&s_acc, out);
    float keep_temp = s_acc.temp_c;
    memset(&s_acc, 0, sizeof(s_acc));
    s_acc.temp_c = keep_temp; /* survives the window; it is a level, not an event */
    xSemaphoreGive(s_lock);
}

void imu_peek(imu_stats_t *out)
{
    if (!s_present) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot(&s_acc, out);
    xSemaphoreGive(s_lock);
}
