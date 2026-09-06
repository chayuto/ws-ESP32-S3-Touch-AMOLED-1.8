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
#include "clog.h"

#include <inttypes.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_nsn_models.h"
#include "esp_vadn_models.h"
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
#include "sdkconfig.h"

static const char *TAG = "recog";

/* MX-1 tallies, printed by recognizer_probe_summary(). */
static uint32_t s_probe_results, s_probe_raw_nonempty, s_probe_raw_differs;
/*
 * Counted separately on purpose. "0 results" from the first run was ambiguous: it
 * could mean MultiNet returns empty results, or that we never reach the branch that
 * reads them. Those need completely different responses, so they get different
 * counters. s_probe_calls counts every entry to probe_log_raw, empty or not.
 */
static uint32_t s_probe_calls, s_state_detected, s_state_timeout, s_state_detecting;
static volatile bool s_hearing; /* last VAD verdict, for the state chip */

#define RECOG_CORE      1
#define FEED_PRIO       6   /* above detect so the ring buffer never starves */
#define DETECT_PRIO     5
#define MN_TIMEOUT_MS   5000
#define BOOK_WORDS_FOR_CLOG 64

/* The neural VAD drops back to silence within a few hundred ms of a word ending, and
 * MultiNet fires ~600 ms after the end; gating on the detection frame alone threw
 * away a 0.94. Speech any time in this window before the detection counts. */
#define SPEECH_HOLD_MS  1500

/*
 * Two gates, and VAD is the one that matters. Digital silence reliably produced
 * 'CAR' at exactly 0.135 with VAD reporting *no speech*; real words fired with
 * VAD still in its speech hangover. Real words ranged 0.28-0.83 across runs, so
 * a probability floor alone would throw away good detections. Keep MIN_PROB low
 * (generosity is the design goal) and let VAD reject the silence junk.
 */
#define MIN_PROB        (CONFIG_DICT_MIN_PROB_PCT / 100.0f)

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static afe_config_t *s_afe_cfg;        /* kept: the AFE is rebuilt from it after maintenance */
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
static volatile bool s_paused;
static volatile bool s_flush;   /* set on resume; detect_task performs the clean() itself */
static const word_def_t *volatile s_pending_words;
static volatile size_t s_pending_count;

static void apply_words(const word_def_t *words, size_t count);

static volatile bool s_stop;
static TaskHandle_t s_feed_task, s_detect_task;
static float s_mic_avg = -120, s_mic_peak = -120;
static int s_mic_speech;

static word_event_t s_raw;
static volatile bool s_raw_valid;

/* Health: cumulative counters and window extremes, written by the audio tasks, read by the app. */
static volatile uint32_t s_h_frames, s_h_timeouts, s_h_mic_errors, s_h_queue_drops;
static volatile float s_h_rb_min = 2, s_h_rb_max = -1;
static volatile int64_t s_h_gap_max_us, s_h_detect_max_us, s_h_last_fetch_us;

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

    while (!s_stop) {
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
                s_h_mic_errors++;
                ESP_LOGE(TAG, "mic read failed: %d", ret);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (s_paused) {
                /* Speaker is busy: keep the AFE's clock running on silence rather than
                 * starving it, which makes it complain about an empty ring buffer. */
                memset(buf, 0, bytes);
            }
        }
        s_afe->feed(s_afe_data, buf);
    }
    free(buf);
    s_feed_task = NULL;
    vTaskDelete(NULL);
}

/*
 * MX-1, the experiment that decides whether 05 is possible.
 *
 * esp_mn_results_t carries two strings beyond the command ids:
 *     string      - the decode snapped to the loaded command list
 *     raw_string  - documented as the decode WITHOUT the commands graph
 *
 * If raw_string carries a free-running phoneme sequence for speech that is not in
 * the command list, then phonemes plus a dictionary on the card is an offline
 * transcript, and MultiNet's 300-command ceiling stops applying. If it is empty or
 * merely mirrors `string`, that path is closed and this project becomes a phrase
 * board. Nobody has published an answer either way, so we log it and look.
 *
 * Printed for every result, detected or timed out, gated or rejected. Deliberately
 * verbose: a probe that filters is a probe that lies.
 */
static void probe_log_raw(const esp_mn_results_t *r, const char *why, float vol_dbfs, int vad, uint32_t frame)
{
    if (r == NULL) {
        return;
    }
    s_probe_calls++;
    bool has_raw = r->raw_string[0] != '\0';
    bool has_str = r->string[0] != '\0';
    if (!has_raw && !has_str && r->num == 0) {
        return; /* nothing at all - do not drown the log in empty timeouts */
    }
    ESP_LOGW(TAG, "MX1 %-8s num=%d vad=%d vol=%.1f frame=%" PRIu32 " string='%s' raw_string='%s'", why, r->num, vad,
             vol_dbfs, frame, has_str ? r->string : "", has_raw ? r->raw_string : "");
    clog_decode(why, r->string, r->raw_string, r->num, r->num > 0 ? r->prob[0] : 0.0f, vad, vol_dbfs, 0, NULL);
    s_probe_results++;
    if (has_raw) {
        s_probe_raw_nonempty++;
        if (!has_str || strcmp(r->raw_string, r->string) != 0) {
            /* The interesting case: raw differs from the command-snapped decode. */
            s_probe_raw_differs++;
        }
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
    int64_t last_speech_us = -1;
    /* Ambient level report every ~5 s: what the recogniser is actually hearing. */
    float lvl_sum = 0, lvl_max = -120;
    uint32_t lvl_n = 0, lvl_speech = 0;
    while (!s_stop) {
        afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(200));
        if (res == NULL || res->ret_value == ESP_FAIL) {
            s_h_timeouts++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        frames++;
        int64_t now_us = esp_timer_get_time();
        if (s_h_last_fetch_us && now_us - s_h_last_fetch_us > s_h_gap_max_us) {
            s_h_gap_max_us = now_us - s_h_last_fetch_us;
        }
        s_h_last_fetch_us = now_us;
        s_h_frames++;
        if (res->ringbuff_free_pct < s_h_rb_min) s_h_rb_min = res->ringbuff_free_pct;
        if (res->ringbuff_free_pct > s_h_rb_max) s_h_rb_max = res->ringbuff_free_pct;
        s_hearing = (res->vad_state == VAD_SPEECH);
        if (res->vad_state == VAD_SPEECH) {
            last_speech_us = esp_timer_get_time();
        }
        if (!s_paused) {
            lvl_sum += res->data_volume;
            if (res->data_volume > lvl_max) {
                lvl_max = res->data_volume;
            }
            lvl_n++;
            lvl_speech += (res->vad_state == VAD_SPEECH);
            if (lvl_n >= 156) { /* 156 x 32 ms = 5 s */
                s_mic_avg = lvl_sum / lvl_n;
                s_mic_peak = lvl_max;
                s_mic_speech = (int)(lvl_speech * 100 / lvl_n);
                ESP_LOGI(TAG, "mic: avg %.1f dBFS, peak %.1f dBFS, vad speech %d%% of last 5 s", s_mic_avg, s_mic_peak, s_mic_speech);
                clog_env(s_mic_avg, s_mic_peak, s_mic_speech);
                lvl_sum = 0; lvl_max = -120; lvl_n = 0; lvl_speech = 0;
            }
        }
        if (s_flush) {
            /* Resume after playback: forget whatever the model had half-heard. Done here,
             * on the task that owns the model, never from another core. */
            s_flush = false;
            s_mn->clean(s_mn_data);
        }
        if (s_pending_words != NULL) {
            const word_def_t *w = s_pending_words;
            size_t n = s_pending_count;
            s_pending_words = NULL;
            ESP_LOGI(TAG, "swapping vocabulary (%u words)", (unsigned)n);
            apply_words(w, n);
            s_mn->clean(s_mn_data);
        }
        if (s_paused) {
            continue; /* frames fed before the pause landed; drop them */
        }

        int64_t det_us = esp_timer_get_time();
        esp_mn_state_t state = s_mn->detect(s_mn_data, res->data);
        if (state == ESP_MN_STATE_DETECTING) {
            s_state_detecting++;
        }
        det_us = esp_timer_get_time() - det_us;
        if (det_us > s_h_detect_max_us) {
            s_h_detect_max_us = det_us;
        }
        if (state == ESP_MN_STATE_DETECTED) {
            s_state_detected++;
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            probe_log_raw(r, "detected", res->data_volume, (int)res->vad_state, frames);
            for (int i = 0; i < r->num; i++) {
                int id = r->command_id[i];
                if (id < 0 || (size_t)id >= s_word_count) {
                    /* Seen from MultiNet7: a second result slot holding an out-of-range id
                     * at prob 0.99. Only slot 0 is ever acted on; this is just noise. */
                    ESP_LOGD(TAG, "detected [%d/%d] junk id=%d ignored", i + 1, r->num, id);
                    continue;
                }
                ESP_LOGI(TAG, "detected [%d/%d] id=%d '%s' prob=%.3f  (vad=%d vol=%.1f dBFS, frame %" PRIu32 ")", i + 1,
                         r->num, id, s_words[id].text, r->prob[i], (int)res->vad_state, res->data_volume, frames);
            }
            bool speech = (res->vad_state == VAD_SPEECH) ||
                          (last_speech_us >= 0 && esp_timer_get_time() - last_speech_us < SPEECH_HOLD_MS * 1000);
            bool valid = r->num > 0 && r->command_id[0] >= 0 && (size_t)r->command_id[0] < s_word_count;
            const char *verdict = !valid ? "invalid" : !speech ? "silence" : r->prob[0] >= MIN_PROB ? "accepted" : "low";
            clog_cand_t cands[ESP_MN_RESULT_MAX_NUM];
            int nc = 0;
            for (int i = 0; i < r->num && i < ESP_MN_RESULT_MAX_NUM; i++) {
                int id = r->command_id[i];
                cands[nc].id = id;
                cands[nc].text = (id >= 0 && (size_t)id < s_word_count) ? s_words[id].text : NULL;
                cands[nc].prob = r->prob[i];
                nc++;
            }
            clog_detection(cands, nc, speech, res->data_volume, verdict, frames);
            if (valid && (!s_raw_valid || r->prob[0] > s_raw.prob)) {
                /* The best guess since the last take, whatever the VAD thought. The self-test
                 * reads it after a clip's silent tail has fed, and the tail always produces a
                 * junk detection at ~0.13 that used to overwrite the real one (DOG 0.66 at
                 * 11.1 s, CAR 0.13 at 12.2 s, judge at 13.3 s). Best-of fixes that; the VAD's
                 * own behaviour is measured by the quiet-room records, not here. */
                s_raw.id = r->command_id[0];
                s_raw.prob = r->prob[0];
                s_raw.text = s_words[s_raw.id].text;
                s_raw_valid = true;
            }
            if (valid && speech && r->prob[0] >= MIN_PROB) {
                word_event_t ev = {.id = r->command_id[0], .prob = r->prob[0],
                                   .text = ((size_t)r->command_id[0] < s_word_count) ? s_words[r->command_id[0]].text : "?"};
                if (xQueueSend(s_out, &ev, 0) != pdTRUE) {
                    s_h_queue_drops++;
                    ESP_LOGW(TAG, "event queue full, dropped '%s'", ev.text);
                }
            } else if (valid) {
                ESP_LOGI(TAG, "rejected: %s (prob %.3f, floor %.2f)", speech ? "low confidence" : "detected on silence",
                         r->prob[0], MIN_PROB);
            }
            s_mn->clean(s_mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            /*
             * MX-1 lives here as much as in the DETECTED branch. Speech that is not in
             * the command list never reaches DETECTED - it times out - so a probe that
             * only looks at detections cannot see out-of-vocabulary decoding at all.
             * 02 discarded this path without reading it. We read it first.
             */
            s_state_timeout++;
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            probe_log_raw(r, "timeout", res->data_volume, (int)res->vad_state, frames);
            /* Continuous listening: a timeout is just "nothing for a while". Reset and carry on. */
            s_mn->clean(s_mn_data);
        }
    }
    s_detect_task = NULL;
    vTaskDelete(NULL);
}

/* Load a word list into MultiNet. Only ever called from the task that owns the model. */
static void apply_words(const word_def_t *words, size_t count)
{
    esp_mn_commands_clear();
    size_t loaded = 0;
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = esp_mn_commands_add((int)i, words[i].text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "word %u '%s' rejected by G2P/model (%s)", (unsigned)i, words[i].text, esp_err_to_name(err));
        } else {
            loaded++;
        }
    }
    esp_mn_error_t *bad = esp_mn_commands_update();
    if (bad != NULL && bad->num > 0) {
        for (int i = 0; i < bad->num; i++) {
            ESP_LOGW(TAG, "command not loadable: '%s'", bad->phrases[i]->string);
        }
    }
    s_words = words;
    s_word_count = count;
    s_mn->print_active_speech_commands(s_mn_data);
    ESP_LOGI(TAG, "vocabulary: %u of %u words loaded", (unsigned)loaded, (unsigned)count);
    const char *texts[BOOK_WORDS_FOR_CLOG];
    size_t nt = count < BOOK_WORDS_FOR_CLOG ? count : BOOK_WORDS_FOR_CLOG;
    for (size_t i = 0; i < nt; i++) {
        texts[i] = words[i].text;
    }
    clog_session(texts, nt, CONFIG_DICT_MIN_PROB_PCT, 0);
}

static void start_tasks(void)
{
    s_h_last_fetch_us = 0; /* a sleep is not a gap */
    s_stop = false;
    s_paused = false;
    s_flush = false;
    s_pending_words = NULL;
    xTaskCreatePinnedToCore(feed_task, "sr_feed", 6144, NULL, FEED_PRIO, &s_feed_task, RECOG_CORE);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8192, NULL, DETECT_PRIO, &s_detect_task, RECOG_CORE);
}

esp_err_t recognizer_start(esp_codec_dev_handle_t mic, const word_def_t *words, size_t count, QueueHandle_t out)
{
    s_mic = mic;
    s_out = out;
    if (s_mn_data != NULL) {
        /* Restart after a stop: MultiNet is live and no task runs. Rebuild the AFE from
         * the kept config, then the words go straight in. */
        if (s_afe_data == NULL) {
            s_afe_data = s_afe->create_from_config(s_afe_cfg);
            ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_FAIL, TAG, "afe re-create failed");
        }
        apply_words(words, count);
        s_afe->reset_buffer(s_afe_data);
        s_mn->clean(s_mn_data);
        start_tasks();
        ESP_LOGI(TAG, "restarted: listening for %u words", (unsigned)count);
        return ESP_OK;
    }
    s_words = words;
    s_word_count = count;

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
    cfg->aec_init = false;              /* recognition is paused during playback instead */
    /* Front end against a lived-in room. Without these the pipeline was
     * [input] -> VAD(WebRTC) -> [output] and the WebRTC VAD called a -52 dBFS room
     * "speech" two thirds of the time; noise then matched short words. */
    /* No noise suppression, on purpose: nsnet2 in this SR pipeline turned both the
     * synthetic clips and a live voice into noise-level junk for MultiNet. */
    cfg->ns_init = false;
    char *vad_model = esp_srmodel_filter(s_models, ESP_VADN_PREFIX, NULL);
    if (vad_model) {
        cfg->vad_init = true;
        cfg->vad_model_name = vad_model;          /* vadnet1_medium, neural */
        cfg->vad_mode = VAD_MODE_2;
        cfg->vad_min_speech_ms = 160;
        cfg->vad_energy_threshold = CONFIG_DICT_VAD_ENERGY_DBFS; /* quiet rooms are not speech */
        ESP_LOGI(TAG, "vad model: %s, energy floor %d dBFS", vad_model, CONFIG_DICT_VAD_ENERGY_DBFS);
    } else {
        ESP_LOGW(TAG, "no vadnet model in the partition; WebRTC VAD");
    }
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
    s_afe_cfg = cfg; /* not freed: see recognizer_stop() */
    s_afe->print_pipeline(s_afe_data);

    /* --- MultiNet --- */
    s_mn = esp_mn_handle_from_name(mn_name);
    ESP_RETURN_ON_FALSE(s_mn != NULL, ESP_FAIL, TAG, "no MultiNet handle for %s", mn_name);
    s_mn_data = s_mn->create(mn_name, MN_TIMEOUT_MS);
    ESP_RETURN_ON_FALSE(s_mn_data != NULL, ESP_FAIL, TAG, "multinet create failed");

    /* Vocabulary: graphemes in, phonemes via the component's own G2P. */
    esp_mn_commands_alloc((esp_mn_iface_t *)s_mn, s_mn_data);
    apply_words(words, count);

    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "engine loaded: internal -%u B (%u free), psram -%u B (%u free)",
             (unsigned)(internal_before - internal_after), (unsigned)internal_after,
             (unsigned)(psram_before - psram_after), (unsigned)psram_after);

    start_tasks();
    ESP_LOGI(TAG, "listening for %u words, no wake word", (unsigned)count);
    return ESP_OK;
}

esp_err_t recognizer_inject(const int16_t *samples, size_t count, const char *label)
{
    if (s_inject_samples != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    while (s_paused || s_flush) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_raw_valid = false; /* judge only what this clip produces */
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

void recognizer_pause(void)
{
    s_paused = true;
}

void recognizer_resume(void)
{
    s_flush = true;
    s_paused = false;
}

void recognizer_set_words(const word_def_t *words, size_t count)
{
    s_pending_count = count;
    s_pending_words = words; /* set last: the detect task polls this */
}

bool recognizer_take_raw(word_event_t *out)
{
    if (!s_raw_valid) {
        return false;
    }
    *out = s_raw;
    s_raw_valid = false;
    return true;
}

void recognizer_stop(void)
{
    if (s_afe_data == NULL) {
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_stop = true;
    for (int i = 0; i < 50 && (s_feed_task || s_detect_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_feed_task || s_detect_task) {
        ESP_LOGW(TAG, "tasks did not exit; deleting them");
        if (s_feed_task) vTaskDelete(s_feed_task);
        if (s_detect_task) vTaskDelete(s_detect_task);
        s_feed_task = s_detect_task = NULL;
    }
    s_inject_samples = NULL;
    s_mic_avg = s_mic_peak = -120; /* no stale level in the state records while stopped */
    s_mic_speech = 0;
    /* MultiNet stays allocated: its destroy() double-frees (assert in tlsf from
     * multinet7_quantized.c:346 with the heap verified clean before the call). The AFE
     * - which holds the VAD model's internal RAM - is destroyed and rebuilt from the
     * kept config on the next start, so maintenance mode has room for Wi-Fi. */
    bool ok = heap_caps_check_integrity_all(true);
    s_afe->destroy(s_afe_data);
    s_afe_data = NULL;
    ESP_LOGI(TAG, "tasks stopped, AFE destroyed (heap %s before, %s after), MultiNet kept; internal +%d B",
             ok ? "ok" : "CORRUPT", heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before));
}

void recognizer_mic_level(float *avg_dbfs, float *peak_dbfs, int *speech_pct)
{
    if (avg_dbfs) *avg_dbfs = s_mic_avg;
    if (peak_dbfs) *peak_dbfs = s_mic_peak;
    if (speech_pct) *speech_pct = s_mic_speech;
}

void recognizer_health(recognizer_health_t *out, bool reset)
{
    out->frames = s_h_frames;
    out->fetch_timeouts = s_h_timeouts;
    out->mic_errors = s_h_mic_errors;
    out->queue_drops = s_h_queue_drops;
    out->rb_min = s_h_rb_max < 0 ? -1 : s_h_rb_min;
    out->rb_max = s_h_rb_max;
    out->gap_max_ms = (uint32_t)(s_h_gap_max_us / 1000);
    out->detect_max_ms = (uint32_t)(s_h_detect_max_us / 1000);
    if (reset) {
        s_h_rb_min = 2;
        s_h_rb_max = -1;
        s_h_gap_max_us = 0;
        s_h_detect_max_us = 0;
    }
}

void recognizer_probe_summary(void)
{
    ESP_LOGW(TAG,
             "MX1 summary: detect states [detecting %" PRIu32 " detected %" PRIu32 " timeout %" PRIu32 "], "
             "probe calls %" PRIu32 ", non-empty %" PRIu32 ", raw_string set %" PRIu32 ", raw differs %" PRIu32,
             s_state_detecting, s_state_detected, s_state_timeout, s_probe_calls, s_probe_results,
             s_probe_raw_nonempty, s_probe_raw_differs);
    if (s_probe_calls == 0) {
        ESP_LOGW(TAG, "MX1: results never read - detect() is not reaching DETECTED or TIMEOUT. Inconclusive, not a verdict.");
    } else if (s_probe_raw_nonempty == 0) {
        ESP_LOGW(TAG, "MX1 verdict so far: raw_string always empty -> option B1 is dead, see docs/design/05_dictation.md");
    } else if (s_probe_raw_differs > 0) {
        ESP_LOGW(TAG, "MX1 verdict so far: raw_string carries something the command graph does not -> B1 is ALIVE, look at it");
    } else {
        ESP_LOGW(TAG, "MX1 verdict so far: raw_string set but always equal to string -> B1 gives nothing extra");
    }
}

bool recognizer_hearing_speech(void)
{
    return s_hearing;
}
