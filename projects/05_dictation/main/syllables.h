#pragma once

#include <stddef.h>

#include "recognizer.h"

/*
 * MX-2: the syllable tables.
 *
 * B1 is dead - MultiNet never fills raw_string - so the only remaining way to get
 * open-ish text out of this engine is to stop asking it for words and ask it for
 * syllables instead. Load the inventory as commands, speak continuously, and see
 * whether it fires syllable after syllable or detects once and times out.
 *
 * MultiNet's documented ceiling is 300 commands, which is the whole reason the two
 * languages are not equivalent here:
 *
 *   Mandarin  ~400 base syllables ignoring tone  -> nearly fits, and the tables below
 *                                                   carry the common core
 *   English   syllable inventory in the thousands -> cannot fit, so the English table
 *                                                   is common monosyllabic WORDS
 *                                                   instead. It cannot test coverage,
 *                                                   but it tests the thing that
 *                                                   actually decides B2: whether the
 *                                                   engine streams.
 *
 * If MultiNet detects one command per utterance and then times out, B2 is dead too
 * and no amount of vocabulary fixes it.
 */

const word_def_t *syllables_table(size_t *count);

/* Which table was compiled in, for the log. */
const char *syllables_kind(void);
