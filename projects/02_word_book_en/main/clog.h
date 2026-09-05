#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Classifier + environment records: JSON Lines on the card beside the log,
 * for analysis on a laptop. One object per line, "t" says which kind:
 *   session   boot / vocabulary / thresholds       (once per start or reload)
 *   det       every MultiNet result, gated or not  (top candidates, vad, level, verdict)
 *   env       every 5 s: ambient level, speech %, heap, card
 * Written only while a card is mounted; the ring buffers otherwise discard.
 */

typedef struct {
    int id;
    const char *text;
    float prob;
} clog_cand_t;

void clog_session(const char *const *words, size_t count, int min_prob_pct, int sound_prob_pct);
void clog_detection(const clog_cand_t *cands, int n, int vad_speech, float volume_dbfs, const char *verdict,
                    uint32_t frame);
void clog_env(float mic_avg_dbfs, float mic_peak_dbfs, int speech_pct);
