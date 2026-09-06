#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * The recogniser is the one module the multilingual follow-on replaces.
 * Everything it tells the rest of the app crosses this boundary:
 */
typedef struct {
    int id;          /* index into the word table handed to recognizer_start() */
    float prob;      /* engine confidence, 0..1 */
    const char *text;
} word_event_t;

typedef struct {
    const char *text; /* the word as written, e.g. "DOG"; the engine derives phonemes itself */
} word_def_t;

/*
 * Start listening. Words are handed to the engine as graphemes; ESP-SR's built-in
 * G2P turns them into phonemes. Events land on `out` as word_event_t.
 */
esp_err_t recognizer_start(esp_codec_dev_handle_t mic, const word_def_t *words, size_t count, QueueHandle_t out);

/*
 * Play a 16 kHz mono clip *into* the recogniser as if the mic heard it. Used by the
 * boot self-test so the engine can be proven without a person in the room.
 * Non-blocking; the clip is consumed by the feed task in place of mic audio.
 */
esp_err_t recognizer_inject(const int16_t *samples, size_t count, const char *label);

/* True while an injected clip is still being fed. */
bool recognizer_injecting(void);

/*
 * The engine's most recent top guess, before the VAD and confidence gates.
 * For the self-test, which checks the pipeline, not the tuning. Returns false
 * if nothing has been detected since the last call.
 */
bool recognizer_take_raw(word_event_t *out);

/*
 * Stop feeding the engine while the speaker is in use, so the board does not
 * hear itself. The mic keeps being drained; resume flushes the AFE and MultiNet.
 */
void recognizer_pause(void);
void recognizer_resume(void);

/*
 * Replace the vocabulary while running. `words` must stay valid until the next
 * call; the swap is performed by the detect task at a safe point, so this
 * returns before the new words are active.
 */
void recognizer_set_words(const word_def_t *words, size_t count);

/* Tear everything down: tasks, MultiNet, the AFE, the model list. Frees ~35 KB internal. */
void recognizer_stop(void);

/*
 * How the audio path is keeping up. Counters are cumulative for the run; the window
 * extremes (rb_*, *_max_ms) cover the time since the last call with `reset` true.
 * ringbuff_free_pct is the AFE's own figure, 0..1, logged as given: which way "busy" goes
 * is learned from the records, not assumed.
 */
typedef struct {
    uint32_t frames;         /* AFE frames fetched (31.25 per second when all is well) */
    uint32_t fetch_timeouts; /* nothing from the AFE within 200 ms */
    uint32_t mic_errors;     /* codec read failures */
    uint32_t queue_drops;    /* words lost because the app's queue was full */
    float rb_min, rb_max;    /* ringbuff_free_pct extremes; -1 when no frame was seen */
    uint32_t gap_max_ms;     /* longest wait between two fetched frames */
    uint32_t detect_max_ms;  /* longest MultiNet detect() call */
} recognizer_health_t;
void recognizer_health(recognizer_health_t *out, bool reset);

/* The engine's last 5-second ambient summary, for metrics. */
void recognizer_mic_level(float *avg_dbfs, float *peak_dbfs, int *speech_pct);
