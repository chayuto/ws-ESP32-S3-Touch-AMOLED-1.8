/*
 * Full-screen cards. A photo card is a 368x448 RGB565 file read straight into
 * a PSRAM buffer and handed to LVGL as an image; no decode step, ~330 KB per
 * card. Two buffers alternate so the previous card stays valid while the next
 * one loads. A text card is the word, large, on a colour picked by index.
 */

#include "cards.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "cards";

#define CARD_W      368
#define CARD_H      448
#define CARD_BYTES  (CARD_W * CARD_H * 2)

static lv_obj_t *s_photo;
static lv_obj_t *s_word;
static lv_obj_t *s_word_shadow;
static lv_obj_t *s_sub;
static lv_obj_t *s_status;

/* Two photo buffers in PSRAM, used alternately. */
static uint8_t *s_buf[2];
static lv_image_dsc_t s_dsc[2];
static int s_next;

static cards_tap_cb_t s_on_tap;

static void screen_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_tap) {
        s_on_tap();
    }
}

static const uint32_t s_palette[] = {0xC2410C, 0x1D4ED8, 0xBE185D, 0x047857, 0x7C3AED, 0xB45309, 0x0E7490, 0x9F1239};

void cards_init(cards_tap_cb_t on_tap)
{
    s_on_tap = on_tap;
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(CARD_BYTES, MALLOC_CAP_SPIRAM);
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "photo buffer %d alloc failed", i);
        }
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = CARD_W;
        s_dsc[i].header.h = CARD_H;
        s_dsc[i].header.stride = CARD_W * 2;
        s_dsc[i].data_size = CARD_BYTES;
        s_dsc[i].data = s_buf[i];
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_pressed_cb, LV_EVENT_PRESSED, NULL);

    s_photo = lv_image_create(scr);
    lv_obj_set_size(s_photo, CARD_W, CARD_H);
    lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_photo, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    /* Word: a dark offset copy underneath makes it readable over any photo. */
    s_word_shadow = lv_label_create(scr);
    lv_obj_set_style_text_font(s_word_shadow, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_word_shadow, lv_color_black(), 0);
    lv_obj_set_style_text_opa(s_word_shadow, LV_OPA_70, 0);

    s_word = lv_label_create(scr);
    lv_obj_set_style_text_font(s_word, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_word, lv_color_white(), 0);

    s_sub = lv_label_create(scr);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0xdddddd), 0);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x99a3ad), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(s_status, "");
}

static void place_word(bool over_photo)
{
    if (over_photo) {
        lv_obj_align(s_word, LV_ALIGN_BOTTOM_MID, 0, -64);
        lv_obj_align(s_word_shadow, LV_ALIGN_BOTTOM_MID, 3, -61);
        lv_obj_align(s_sub, LV_ALIGN_BOTTOM_MID, 0, -36);
    } else {
        lv_obj_align(s_word, LV_ALIGN_CENTER, 0, -10);
        lv_obj_align(s_word_shadow, LV_ALIGN_CENTER, 3, -7);
        lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 44);
    }
}

void cards_show_idle(void)
{
    if (!bsp_display_lock(300)) {
        return;
    }
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1f2937), 0);
    lv_label_set_text(s_word, "say a word");
    lv_label_set_text(s_word_shadow, "say a word");
    lv_label_set_text(s_sub, "");
    place_word(false);
    bsp_display_unlock();
}

static bool load_photo(const char *path, uint8_t *dst)
{
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return false;
    }
    size_t got = 0;
    while (got < CARD_BYTES) {
        size_t n = fread(dst + got, 1, CARD_BYTES - got, f);
        if (n == 0) {
            break;
        }
        got += n;
    }
    fclose(f);
    if (got != CARD_BYTES) {
        ESP_LOGW(TAG, "%s: %u bytes, expected %u", path, (unsigned)got, (unsigned)CARD_BYTES);
        return false;
    }
    ESP_LOGI(TAG, "photo %s loaded in %" PRId64 " ms", path, (esp_timer_get_time() - t0) / 1000);
    return true;
}

bool cards_show_word(const book_word_t *word, float confidence, unsigned nth)
{
    char sub[32];
    snprintf(sub, sizeof(sub), "%.0f%%   #%u", confidence * 100.0f, nth);

    bool photo = false;
    int slot = s_next;
    if (word->photo[0] && s_buf[slot] != NULL) {
        photo = load_photo(word->photo, s_buf[slot]);
    }

    if (!bsp_display_lock(300)) {
        return photo;
    }
    if (photo) {
        lv_image_set_src(s_photo, &s_dsc[slot]);
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        s_next ^= 1;
    } else {
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        uint32_t idx = 0;
        for (const char *p = word->text; *p; p++) {
            idx = idx * 31 + (uint8_t)*p;
        }
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(s_palette[idx % (sizeof(s_palette) / sizeof(s_palette[0]))]), 0);
    }
    lv_label_set_text(s_word, word->text);
    lv_label_set_text(s_word_shadow, word->text);
    lv_label_set_text(s_sub, sub);
    place_word(photo);
    bsp_display_unlock();
    return photo;
}

void cards_status(const char *text)
{
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_status, text);
        bsp_display_unlock();
    }
}
