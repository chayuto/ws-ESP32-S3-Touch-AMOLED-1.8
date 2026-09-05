/*
 * ESP-SR pipeline: mic -> AFE (NS, VAD, AGC) -> MultiNet7 English -> word_event_t.
 *
 * Two tasks, both on core 1, in the shape ESP-SR expects:
 *   feed_task    reads one AFE chunk from the mic (or an injected clip) and feeds it
 *   detect_task  fetches the enhanced audio and runs MultiNet on it
 *
 * There is deliberately no wake word. The AFE is created with wakenet_init = false
 * and MultiNet runs on every fetched frame. The docs call WakeNet required; M1
 * exists to find out whether that is a rule or a recommendation.
 */

#include "recognizer.h"

#include "audio_io.h"

#include <inttypes.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "recog";

#define RECOG_CORE      1
#define FEED_PRIO       6   /* above detect so the ring buffer never starves */
#define DETECT_PRIO     5
#define MN_TIMEOUT_MS   5000

/*
 * Two gates, and VAD is the one that matters. Digital silence reliably produced
 * 'CAR' at exactly 0.135 with VAD reporting *no speech*; real words fired with
 * VAD still in its speech hangover. Real words ranged 0.28-0.83 across runs, so
 * a probability floor alone would throw away good detections. Keep MIN_PROB low
 * (generosity is the design goal) and let VAD reject the silence junk.
 */
#define MIN_PROB        0.20f

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static srmodel_list_t *s_models;

static esp_codec_dev_handle_t s_mic;
static const word_def_t *s_words;
static size_t s_word_count;
static QueueHandle_t s_out;

/* Injection: a clip the feed task consumes instead of the mic. */
static const int16_t *s_inject_samples;
static size_t s_inject_count;
static size_t s_inject_pos;
static const char *s_inject_label;
static int64_t s_inject_started_us;

static void feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    const int channels = s_afe->get_feed_channel_num(s_afe_data);
    const size_t bytes = (size_t)chunk * channels * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "feed: %d samples x %d ch per chunk (%u bytes), core %d", chunk, channels, (unsigned)bytes,
             xPortGetCoreID());

    for (;;) {
        if (s_inject_samples != NULL) {
            /* Feed the clip chunk by chunk, zero-padding the tail. */
            size_t left = s_inject_count - s_inject_pos;
            size_t n = left < (size_t)chunk ? left : (size_t)chunk;
            memcpy(buf, s_inject_samples + s_inject_pos, n * sizeof(int16_t));
            if (n < (size_t)chunk) {
                memset(buf + n, 0, (chunk - n) * sizeof(int16_t));
            }
            s_inject_pos += n;
            if (s_inject_pos >= s_inject_count) {
                ESP_LOGI(TAG, "inject '%s': %u samples fed in %" PRId64 " ms", s_inject_label,
                         (unsigned)s_inject_count, (esp_timer_get_time() - s_inject_started_us) / 1000);
                s_inject_samples = NULL;
            }
            /* Pace it like real time so the AFE ring buffer does not overflow. */
            vTaskDelay(pdMS_TO_TICKS(chunk * 1000 / AUDIO_SAMPLE_RATE_HZ));
        } else {
            int ret = esp_codec_dev_read(s_mic, buf, (int)bytes);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "mic read failed: %d", ret);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }
        s_afe->feed(s_afe_data, buf);
    }
}

static void detect_task(void *arg)
{
    (void)arg;
    const int afe_chunk = s_afe->get_fetch_chunksize(s_afe_data);
    const int mn_chunk = s_mn->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "detect: afe fetch %d samples, multinet wants %d, core %d", afe_chunk, mn_chunk, xPortGetCoreID());
    if (afe_chunk != mn_chunk) {
        ESP_LOGW(TAG, "chunk mismatch; MultiNet will see partial frames");
    }

    uint32_t frames = 0;
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "afe fetch failed");
            continue;
        }
        frames++;

        esp_mn_state_t state = s_mn->detect(s_mn_data, res->data);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            for (int i = 0; i < r->num; i++) {
                int id = r->command_id[i];
                const char *text = (id >= 0 && (size_t)id < s_word_count) ? s_words[id].text : "?";
                ESP_LOGI(TAG, "detected [%d/%d] id=%d '%s' prob=%.3f  (vad=%d vol=%.1f dBFS, frame %" PRIu32 ")", i + 1,
                         r->num, id, text, r->prob[i], (int)res->vad_state, res->data_volume, frames);
            }
            bool speech = (res->vad_state == VAD_SPEECH);
            if (r->num > 0 && speech && r->prob[0] >= MIN_PROB) {
                word_event_t ev = {.id = r->command_id[0], .prob = r->prob[0],
                                   .text = ((size_t)r->command_id[0] < s_word_count) ? s_words[r->command_id[0]].text : "?"};
                if (xQueueSend(s_out, &ev, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "event queue full, dropped '%s'", ev.text);
                }
            } else if (r->num > 0) {
                ESP_LOGI(TAG, "rejected: %s (prob %.3f, floor %.2f)", speech ? "low confidence" : "detected on silence",
                         r->prob[0], MIN_PROB);
            }
            s_mn->clean(s_mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            /* Continuous listening: a timeout is just "nothing for a while". Reset and carry on. */
            s_mn->clean(s_mn_data);
        }
    }
}

esp_err_t recognizer_start(esp_codec_dev_handle_t mic, const word_def_t *words, size_t count, QueueHandle_t out)
{
    s_mic = mic;
    s_words = words;
    s_word_count = count;
    s_out = out;

    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* Models live in the `model` partition; flashed by the build alongside the app. */
    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models != NULL, ESP_FAIL, TAG, "no models in partition 'model'");
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "model[%d]: %s", i, s_models->model_name[i]);
    }

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    ESP_RETURN_ON_FALSE(mn_name != NULL, ESP_FAIL, TAG, "no English MultiNet model in partition");
    ESP_LOGI(TAG, "multinet model: %s", mn_name);

    /* --- AFE: single mic, speech-recognition type, no wake word --- */
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_FAIL, TAG, "afe_config_init failed");
    cfg->wakenet_init = false;
    cfg->aec_init = false;              /* nothing to cancel yet; playback muting comes in M2 */
    cfg->afe_perferred_core = RECOG_CORE;
    cfg->afe_perferred_priority = DETECT_PRIO;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->pcm_config.sample_rate = AUDIO_SAMPLE_RATE_HZ;
    afe_config_check(cfg);
    afe_config_print(cfg);

    s_afe = esp_afe_handle_from_config(cfg);
    ESP_RETURN_ON_FALSE(s_afe != NULL, ESP_FAIL, TAG, "no AFE handle for config");
    s_afe_data = s_afe->create_from_config(cfg);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_FAIL, TAG, "afe create failed");
    afe_config_free(cfg);
    s_afe->print_pipeline(s_afe_data);

    /* --- MultiNet --- */
    s_mn = esp_mn_handle_from_name(mn_name);
    ESP_RETURN_ON_FALSE(s_mn != NULL, ESP_FAIL, TAG, "no MultiNet handle for %s", mn_name);
    s_mn_data = s_mn->create(mn_name, MN_TIMEOUT_MS);
    ESP_RETURN_ON_FALSE(s_mn_data != NULL, ESP_FAIL, TAG, "multinet create failed");

    /* Vocabulary: graphemes in, phonemes via the component's own G2P. */
    esp_mn_commands_alloc((esp_mn_iface_t *)s_mn, s_mn_data);
    esp_mn_commands_clear();
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = esp_mn_commands_add((int)i, words[i].text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "word %u '%s' rejected by G2P/model (%s)", (unsigned)i, words[i].text, esp_err_to_name(err));
        }
    }
    esp_mn_error_t *bad = esp_mn_commands_update();
    if (bad != NULL && bad->num > 0) {
        for (int i = 0; i < bad->num; i++) {
            ESP_LOGW(TAG, "command not loadable: '%s'", bad->phrases[i]->string);
        }
    }
    s_mn->print_active_speech_commands(s_mn_data);

    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "engine loaded: internal -%u B (%u free), psram -%u B (%u free)",
             (unsigned)(internal_before - internal_after), (unsigned)internal_after,
             (unsigned)(psram_before - psram_after), (unsigned)psram_after);

    xTaskCreatePinnedToCore(feed_task, "sr_feed", 6144, NULL, FEED_PRIO, NULL, RECOG_CORE);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8192, NULL, DETECT_PRIO, NULL, RECOG_CORE);
    ESP_LOGI(TAG, "listening for %u words, no wake word", (unsigned)count);
    return ESP_OK;
}

esp_err_t recognizer_inject(const int16_t *samples, size_t count, const char *label)
{
    if (s_inject_samples != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_inject_pos = 0;
    s_inject_count = count;
    s_inject_label = label;
    s_inject_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "inject '%s': %u samples (%.2f s)", label, (unsigned)count, (float)count / AUDIO_SAMPLE_RATE_HZ);
    s_inject_samples = samples; /* set last: the feed task polls this */
    return ESP_OK;
}

bool recognizer_injecting(void)
{
    return s_inject_samples != NULL;
}
