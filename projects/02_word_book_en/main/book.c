/*
 * The vocabulary is data. words.json on the SD card decides what the board
 * listens for and what it shows; the firmware never changes when the book does.
 */

#include "book.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "book";

/* What the board knows with no card in it. Text cards only. */
static const char *const s_builtin[] = {"DOG", "CAT", "BALL", "DUCK", "BABY", "CAR", "SHOE", "BOOK", "BIRD", "APPLE"};

static void load_builtin(book_t *book)
{
    memset(book, 0, sizeof(*book));
    for (size_t i = 0; i < sizeof(s_builtin) / sizeof(s_builtin[0]) && i < BOOK_MAX_WORDS; i++) {
        strlcpy(book->words[i].text, s_builtin[i], BOOK_TEXT_LEN);
        book->count++;
    }
    book->from_sd = false;
    ESP_LOGI(TAG, "built-in vocabulary: %u words, text cards only", (unsigned)book->count);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static bool resolve(char *dst, size_t dst_len, const char *dir, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        dst[0] = '\0';
        return false;
    }
    snprintf(dst, dst_len, "%s/%s", dir, name);
    struct stat st;
    if (stat(dst, &st) != 0) {
        ESP_LOGW(TAG, "listed but missing: %s", dst);
        dst[0] = '\0';
        return false;
    }
    return true;
}

static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s -= 'a' - 'A';
        }
    }
}

void book_load(book_t *book, const char *dir)
{
    char path[BOOK_PATH_LEN];
    snprintf(path, sizeof(path), "%s/words.json", dir);

    size_t len = 0;
    char *json = read_file(path, &len);
    if (json == NULL) {
        ESP_LOGW(TAG, "no %s", path);
        load_builtin(book);
        return;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    cJSON *words = root ? cJSON_GetObjectItem(root, "words") : NULL;
    if (!cJSON_IsArray(words)) {
        ESP_LOGE(TAG, "%s: no 'words' array", path);
        cJSON_Delete(root);
        load_builtin(book);
        return;
    }

    memset(book, 0, sizeof(*book));
    int photos = 0, prompts = 0;
    cJSON *w;
    cJSON_ArrayForEach(w, words) {
        if (book->count >= BOOK_MAX_WORDS) {
            ESP_LOGW(TAG, "more than %d words; rest ignored", BOOK_MAX_WORDS);
            break;
        }
        cJSON *text = cJSON_GetObjectItem(w, "text");
        if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
            continue;
        }
        book_word_t *bw = &book->words[book->count];
        strlcpy(bw->text, text->valuestring, BOOK_TEXT_LEN);
        upper(bw->text);
        cJSON *photo = cJSON_GetObjectItem(w, "photo");
        cJSON *prompt = cJSON_GetObjectItem(w, "prompt");
        photos += resolve(bw->photo, sizeof(bw->photo), dir, cJSON_IsString(photo) ? photo->valuestring : NULL);
        prompts += resolve(bw->prompt, sizeof(bw->prompt), dir, cJSON_IsString(prompt) ? prompt->valuestring : NULL);
        ESP_LOGI(TAG, "  %-10s photo:%s prompt:%s", bw->text, bw->photo[0] ? "yes" : "-", bw->prompt[0] ? "yes" : "-");
        book->count++;
    }
    cJSON_Delete(root);

    if (book->count == 0) {
        ESP_LOGW(TAG, "%s has no usable words", path);
        load_builtin(book);
        return;
    }
    book->from_sd = true;
    ESP_LOGI(TAG, "loaded %s: %u words, %d photos, %d prompts", path, (unsigned)book->count, photos, prompts);
}

bool book_same_words(const book_t *a, const book_t *b)
{
    if (a->count != b->count) {
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(a->words[i].text, b->words[i].text) != 0) {
            return false;
        }
    }
    return true;
}

void book_adopt_files(book_t *dst, const book_t *src)
{
    for (size_t i = 0; i < dst->count && i < src->count; i++) {
        strlcpy(dst->words[i].photo, src->words[i].photo, BOOK_PATH_LEN);
        strlcpy(dst->words[i].prompt, src->words[i].prompt, BOOK_PATH_LEN);
    }
    dst->from_sd = src->from_sd;
}
