#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BOOK_MAX_WORDS   32
#define BOOK_TEXT_LEN    24
#define BOOK_PATH_LEN    96

typedef struct {
    char text[BOOK_TEXT_LEN];    /* the word as recognised and displayed, upper case */
    char photo[BOOK_PATH_LEN];   /* full path to a 368x448 RGB565 file, or "" */
    char prompt[BOOK_PATH_LEN];  /* full path to a 16 kHz mono WAV, or "" */
} book_word_t;

typedef struct {
    book_word_t words[BOOK_MAX_WORDS];
    size_t count;
    bool from_sd;                /* false: built-in starter vocabulary, text cards only */
} book_t;

/*
 * Load `<dir>/words.json` and resolve its file references. On any failure the
 * built-in starter vocabulary is loaded instead and `from_sd` is false, so the
 * app always has something to listen for.
 */
void book_load(book_t *book, const char *dir);

/* True if both books list the same words in the same order (files may differ). */
bool book_same_words(const book_t *a, const book_t *b);

/* Copy photo/prompt paths from src into dst, word by word. Only valid when same_words. */
void book_adopt_files(book_t *dst, const book_t *src);
