/*
 * The vocabulary is data. words.json on the SD card decides what the board
 * listens for and what it shows; the firmware never changes when the book does.
 */

#include "book.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "book";

/* What the board knows with no card in it. Text cards only. */
static const char *const s_builtin[] = {"DOG",  "CAT",   "BALL", "DUCK", "BABY", "CAR",   "SHOE", "BOOK", "BIRD", "APPLE", "BEE",
                                        "ONE",  "TWO",   "THREE", "FOUR", "FIVE", "SIX",  "SEVEN", "EIGHT", "NINE"};

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

static book_word_t *find_or_add(book_t *book, const char *text)
{
    for (size_t i = 0; i < book->count; i++) {
        if (strcmp(book->words[i].text, text) == 0) {
            return &book->words[i];
        }
    }
    if (book->count >= BOOK_MAX_WORDS) {
        return NULL;
    }
    book_word_t *bw = &book->words[book->count++];
    memset(bw, 0, sizeof(*bw));
    strlcpy(bw->text, text, BOOK_TEXT_LEN);
    return bw;
}

/* "dog.jpg" -> "DOG"; "ice_cream.JPG" -> "ICE CREAM". Returns false for names that are not words. */
static bool word_from_name(const char *name, char *out)
{
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n == 0 || n >= BOOK_TEXT_LEN || name[0] == '.' || name[0] == '_') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '_' || c == '-') {
            out[i] = ' ';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ') {
            out[i] = c;
        } else {
            return false;
        }
    }
    out[n] = '\0';
    upper(out);
    return true;
}

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

/*
 * The book is whatever is in the folder. Every dog.jpg / dog.jpeg / dog.rgb565 is
 * a word with a photo; every dog.wav is a prompt. words.json, if present, adds
 * text-only words and can point a word at a differently named file, but nobody
 * needs to write it: dropping photos into the folder is the whole job.
 */
static void scan_dir(book_t *book, const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char word[BOOK_TEXT_LEN];
        bool photo = has_ext(e->d_name, ".jpg") || has_ext(e->d_name, ".jpeg") || has_ext(e->d_name, ".rgb565");
        bool prompt = has_ext(e->d_name, ".wav");
        if (!(photo || prompt) || !word_from_name(e->d_name, word)) {
            if (has_ext(e->d_name, ".png") || has_ext(e->d_name, ".heic")) {
                ESP_LOGW(TAG, "%s: only .jpg photos are read; skipped", e->d_name);
            }
            continue;
        }
        book_word_t *bw = find_or_add(book, word);
        if (bw == NULL) {
            ESP_LOGW(TAG, "more than %d words; %s ignored", BOOK_MAX_WORDS, e->d_name);
            continue;
        }
        char *slot = photo ? bw->photo : bw->prompt;
        /* A .jpg beats a stale .rgb565 of the same word; the loader uses the cache itself. */
        if (photo && slot[0] && has_ext(slot, ".rgb565") && !has_ext(e->d_name, ".rgb565")) {
            slot[0] = '\0';
        }
        if (slot[0] == '\0') {
            if (strlen(dir) + 1 + strlen(e->d_name) >= BOOK_PATH_LEN) {
                ESP_LOGW(TAG, "%s: name too long; skipped", e->d_name);
                continue;
            }
            strlcpy(slot, dir, BOOK_PATH_LEN);
            strlcat(slot, "/", BOOK_PATH_LEN);
            strlcat(slot, e->d_name, BOOK_PATH_LEN);
        }
    }
    closedir(d);
}

static void apply_manifest(book_t *book, const char *dir)
{
    char path[BOOK_PATH_LEN];
    snprintf(path, sizeof(path), "%s/words.json", dir);
    size_t len = 0;
    char *json = read_file(path, &len);
    if (json == NULL) {
        return;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    cJSON *words = root ? cJSON_GetObjectItem(root, "words") : NULL;
    if (!cJSON_IsArray(words)) {
        ESP_LOGW(TAG, "%s: no 'words' array; ignored", path);
        cJSON_Delete(root);
        return;
    }
    cJSON *w;
    cJSON_ArrayForEach(w, words) {
        cJSON *text = cJSON_GetObjectItem(w, "text");
        if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
            continue;
        }
        char t[BOOK_TEXT_LEN];
        strlcpy(t, text->valuestring, sizeof(t));
        upper(t);
        book_word_t *bw = find_or_add(book, t);
        if (bw == NULL) {
            break;
        }
        cJSON *photo = cJSON_GetObjectItem(w, "photo");
        cJSON *prompt = cJSON_GetObjectItem(w, "prompt");
        if (cJSON_IsString(photo) && !bw->photo[0]) {
            resolve(bw->photo, sizeof(bw->photo), dir, photo->valuestring);
        }
        if (cJSON_IsString(prompt) && !bw->prompt[0]) {
            resolve(bw->prompt, sizeof(bw->prompt), dir, prompt->valuestring);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "words.json applied");
}

void book_load(book_t *book, const char *dir)
{
    memset(book, 0, sizeof(*book));
    scan_dir(book, dir);
    apply_manifest(book, dir);

    if (book->count == 0) {
        ESP_LOGW(TAG, "nothing in %s", dir);
        load_builtin(book);
        return;
    }
    int photos = 0, prompts = 0;
    for (size_t i = 0; i < book->count; i++) {
        book_word_t *bw = &book->words[i];
        photos += bw->photo[0] != '\0';
        prompts += bw->prompt[0] != '\0';
        ESP_LOGI(TAG, "  %-12s %s%s", bw->text, bw->photo[0] ? strrchr(bw->photo, '/') + 1 : "(text card)",
                 bw->prompt[0] ? "  + prompt" : "");
    }
    book->from_sd = true;
    ESP_LOGI(TAG, "loaded %s: %u words, %d photos, %d prompts", dir, (unsigned)book->count, photos, prompts);
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
