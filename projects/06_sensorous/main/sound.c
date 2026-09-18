#include "sound.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "sound";

/*
 * 16 kHz mono. A level does not need bandwidth, but this is the rate the rest of
 * the repo runs the ES8311 at and the rate its bring-up was verified at
 * (02_word_book_en, then 05_dictation), so it is the rate least likely to
 * surprise anyone.
 */
#define SAMPLE_RATE_HZ 16000

/* The only audio buffer in the program. 512 samples, overwritten every read. */
#define CHUNK_SAMPLES 512
static int16_t s_chunk[CHUNK_SAMPLES];

#define FULL_SCALE 32768.0f
#define FLOOR_DBFS (-120.0f)
#define CLIP_LEVEL 32000 /* an ES8311 at 30 dB gain sits well below this unless something is loud */

static esp_codec_dev_handle_t s_mic;
static bool s_present;
static bool s_open;
static sound_level_t s_last;
static uint32_t s_bursts, s_failures;
static SemaphoreHandle_t s_lock;

static float to_dbfs(float amplitude)
{
    if (amplitude <= 0.0f) {
        return FLOOR_DBFS;
    }
    float db = 20.0f * log10f(amplitude / FULL_SCALE);
    return db < FLOOR_DBFS ? FLOOR_DBFS : db;
}

esp_err_t sound_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_last.rms_dbfs = s_last.peak_dbfs = FLOOR_DBFS;

    /* The BSP defaults to 22,050 Hz; hand it ours. Speaker pins are configured
     * because bsp_audio_init wants a full pin set, but the speaker codec is
     * never initialised and the amplifier enable on GPIO 46 is never asserted:
     * this project has no reason to make a sound. */
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "bsp_audio_init");

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "ES8311 microphone did not initialise; the sound field will be absent from every record");
        return ESP_FAIL;
    }
    s_present = true;

    /* Prove it opens once, at boot, so a failure is a boot-log line rather than
     * a silent null in the records an hour later. Then close it again. */
    sound_level_t probe;
    bool ok = sound_measure(&probe);
    ESP_LOGI(TAG, "ready: %d Hz mono, gain %d dB, %d ms bursts after a %d ms settle. First reading %s%.1f dBFS",
             SAMPLE_RATE_HZ, CONFIG_SENSOROUS_MIC_GAIN_DB, CONFIG_SENSOROUS_SOUND_MS,
             CONFIG_SENSOROUS_SOUND_SETTLE_MS, ok ? "" : "FAILED at ", (double)probe.rms_dbfs);
    ESP_LOGI(TAG, "the microphone is closed between bursts; nothing is captured in between and no audio is stored");
    return ok ? ESP_OK : ESP_FAIL;
}

bool sound_present(void)
{
    return s_present;
}

static bool mic_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE_HZ,
        .mclk_multiple = 256,
    };
    /* esp_codec_dev_open() disables the I2S channel before configuring it, and
     * the channel is already disabled because the last burst closed it. The
     * driver logs "i2s_channel_disable(): the channel has not been enabled yet"
     * at E for that, once per burst - six times a minute into the card log, for
     * a condition this code creates on purpose. Muted across the open only, so
     * a real I2S error during a read still reaches the log. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    esp_err_t open_err = esp_codec_dev_open(s_mic, &fs);
    esp_log_level_set("i2s_common", ESP_LOG_INFO);
    if (open_err != ESP_CODEC_DEV_OK) {
        return false;
    }
    esp_codec_dev_set_in_gain(s_mic, (float)CONFIG_SENSOROUS_MIC_GAIN_DB);
    s_open = true;
    return true;
}

static void mic_close(void)
{
    if (s_open) {
        esp_codec_dev_close(s_mic);
        s_open = false;
    }
}

/* Read and throw away, so the ADC's start-up transient is not the measurement. */
static void discard(int samples)
{
    while (samples > 0) {
        int n = samples < CHUNK_SAMPLES ? samples : CHUNK_SAMPLES;
        if (esp_codec_dev_read(s_mic, s_chunk, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return;
        }
        samples -= n;
    }
}

bool sound_measure(sound_level_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rms_dbfs = out->peak_dbfs = FLOOR_DBFS;
    if (!s_present) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();

    if (!mic_open()) {
        xSemaphoreGive(s_lock);
        s_failures++;
        ESP_LOGW(TAG, "microphone would not open");
        return false;
    }
    discard(SAMPLE_RATE_HZ * CONFIG_SENSOROUS_SOUND_SETTLE_MS / 1000);

    /* int64 sums: 3,200 samples x 32768^2 is 3.4e12, which does not fit in 32 bits. */
    int64_t sum = 0, sumsq = 0;
    uint32_t n = 0, clipped = 0, errors = 0;
    int16_t vmin = 32767, vmax = -32768;
    const int want = SAMPLE_RATE_HZ * CONFIG_SENSOROUS_SOUND_MS / 1000;

    while ((int)n < want) {
        int chunk = (want - (int)n) < CHUNK_SAMPLES ? (want - (int)n) : CHUNK_SAMPLES;
        if (esp_codec_dev_read(s_mic, s_chunk, chunk * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            errors++;
            break;
        }
        for (int i = 0; i < chunk; i++) {
            int v = s_chunk[i];
            sum += v;
            sumsq += (int64_t)v * v;
            if (v < vmin) vmin = (int16_t)v;
            if (v > vmax) vmax = (int16_t)v;
            if (v >= CLIP_LEVEL || v <= -CLIP_LEVEL) {
                clipped++;
            }
        }
        n += (uint32_t)chunk;
    }

    mic_close();
    /* The samples are gone the moment the next burst starts; make it explicit
     * here as well, so nothing downstream can ever reach a stale window. */
    memset(s_chunk, 0, sizeof(s_chunk));

    out->samples = n;
    out->clipped = clipped;
    out->errors = errors;
    out->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (n > 0) {
        /* Remove the DC offset before the RMS. A MEMS mic through a codec sits on
         * a bias; measuring the level without subtracting it measures the bias. */
        double mean = (double)sum / n;
        double var = (double)sumsq / n - mean * mean;
        if (var < 0) {
            var = 0; /* rounding, at very low levels */
        }
        out->rms_dbfs = to_dbfs((float)sqrt(var));

        double hi = (double)vmax - mean, lo = mean - (double)vmin;
        out->peak_dbfs = to_dbfs((float)(hi > lo ? hi : lo));
        out->valid = true;
    }

    s_bursts++;
    if (!out->valid) {
        s_failures++;
    }
    s_last = *out;
    xSemaphoreGive(s_lock);

    ESP_LOGD(TAG, "burst: %.1f dBFS rms, %.1f peak, %u samples, %u clipped, %u ms", (double)out->rms_dbfs,
             (double)out->peak_dbfs, (unsigned)n, (unsigned)clipped, (unsigned)out->ms);
    if (clipped > n / 20) {
        ESP_LOGW(TAG, "%u of %u samples clipped - the reading is a floor, not a level. Lower the mic gain.",
                 (unsigned)clipped, (unsigned)n);
    }
    return out->valid;
}

void sound_last(sound_level_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_lock);
}

void sound_counts(uint32_t *bursts, uint32_t *failures)
{
    if (bursts) *bursts = s_bursts;
    if (failures) *failures = s_failures;
}
