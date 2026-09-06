/*
 * Text-only screen. 368x448, three stacked regions: a state chip, a scrolling
 * transcript, and a detail line.
 *
 * The panel control at the top of this file is carried over from
 * 02_word_book_en/cards.c verbatim, comments included, because both of the
 * comments describe a fault that cost a day. Do not simplify them away.
 */

#include "display.h"

#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define SCREEN_W 368
#define SCREEN_H 448

static lv_obj_t *s_chip;
static lv_obj_t *s_transcript;
static lv_obj_t *s_text;
static lv_obj_t *s_detail;
static lv_obj_t *s_msg_title;
static lv_obj_t *s_msg_detail;
static lv_obj_t *s_msg_panel;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

/* The transcript is capped so a long session cannot grow the label without bound. */
#define TRANSCRIPT_MAX 2048
static char s_buf[TRANSCRIPT_MAX];
static size_t s_len;

/*
 * Brightness, checked. bsp_display_brightness_set() sends MIPI 0x51 over the QSPI bus
 * and then `return ESP_OK;` unconditionally - it discards the transmit result. On
 * 2026-09-06 that cost us an afternoon: the panel sat black while every log line said
 * the screen was on, because a failed write reported success. Same command, same
 * 0x02<<24 QSPI command-mode flag, but we look at what the bus says.
 */
esp_err_t display_set_brightness(int pct)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pct < 0) { pct = 0; }
    if (pct > 100) { pct = 100; }
    uint8_t param = (uint8_t)(pct * 255 / 100);
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, lcd_cmd, &param, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "brightness %d%% NOT written: %s", pct, esp_err_to_name(err));
    }
    return err;
}

/*
 * A real display off. Writing brightness 0 leaves the CO5300 powered and displaying
 * black; a later 0x51 did not reliably bring it back, and the only recovery was the
 * full re-init below. Pair this with display_panel_reinit() on wake, never with a
 * bare brightness write.
 */
esp_err_t display_off(void)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t a = display_set_brightness(0);
    esp_err_t b = esp_lcd_panel_disp_on_off(s_panel, false);
    ESP_LOGI(TAG, "display off: brightness %s, disp_off %s", esp_err_to_name(a), esp_err_to_name(b));
    return b != ESP_OK ? b : a;
}

void display_force_bright(void)
{
    esp_err_t err = display_set_brightness(100);
    ESP_LOGI(TAG, "brightness 100: %s", esp_err_to_name(err));
    if (bsp_display_lock(300)) {
        lv_obj_invalidate(lv_screen_active());
        bsp_display_unlock();
    }
}

/* There is no panel reset line on this board (LCD RST is GPIO_NUM_NC), so re-running
 * the init sequence in software is the only recovery from a wedged panel. */
void display_panel_reinit(void)
{
    if (s_panel == NULL) {
        ESP_LOGW(TAG, "no panel handle");
        return;
    }
    esp_err_t a = esp_lcd_panel_init(s_panel);
    esp_err_t b = esp_lcd_panel_disp_on_off(s_panel, true);
    esp_err_t c = display_set_brightness(100);
    ESP_LOGI(TAG, "panel re-init: init %s, disp_on %s, brightness %s", esp_err_to_name(a), esp_err_to_name(b),
             esp_err_to_name(c));
    if (bsp_display_lock(300)) {
        lv_obj_invalidate(lv_screen_active());
        bsp_display_unlock();
    }
}

#define DRAW_LINES 20 /* 368 x 20 x 2 = 14,720 B, internal DMA */

lv_display_t *display_start(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    bsp_display_config_t dcfg = {0};
    if (bsp_display_new(&dcfg, &panel, &io) != ESP_OK) {
        return NULL;
    }
    s_panel = panel;
    s_io = io;
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = SCREEN_W * DRAW_LINES,
        .double_buffer = false,
        .hres = SCREEN_W,
        .vres = SCREEN_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true, .buff_spiram = false, .sw_rotate = false, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        return NULL;
    }
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp) {
        const lvgl_port_touch_cfg_t tcfg = {.disp = disp, .handle = tp};
        lvgl_port_add_touch(&tcfg);
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }
    esp_err_t berr = bsp_display_brightness_init();
    ESP_LOGI(TAG, "panel up: brightness init %s", esp_err_to_name(berr));
    return disp;
}

void display_init(void)
{
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "no display lock; screen not built");
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* State chip, top. Small, always present, never blank. */
    s_chip = lv_label_create(scr);
    lv_label_set_text(s_chip, "starting");
    lv_obj_set_style_text_font(s_chip, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_chip, lv_color_hex(0x8899aa), 0);
    lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, 10);

    /* Transcript, middle, scrollable. */
    s_transcript = lv_obj_create(scr);
    lv_obj_set_size(s_transcript, SCREEN_W - 24, SCREEN_H - 96);
    lv_obj_align(s_transcript, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(s_transcript, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(s_transcript, 0, 0);
    lv_obj_set_style_pad_all(s_transcript, 10, 0);
    lv_obj_set_style_radius(s_transcript, 8, 0);

    s_text = lv_label_create(s_transcript);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, SCREEN_W - 48);
    lv_label_set_text(s_text, "");
    lv_obj_set_style_text_font(s_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_text, lv_color_white(), 0);

    /* Detail line, bottom. */
    s_detail = lv_label_create(scr);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail, SCREEN_W - 24);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(0x778899), 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Full-screen message panel, hidden until needed. */
    s_msg_panel = lv_obj_create(scr);
    lv_obj_set_size(s_msg_panel, SCREEN_W, SCREEN_H);
    lv_obj_center(s_msg_panel);
    lv_obj_set_style_bg_color(s_msg_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_msg_panel, 0, 0);
    lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_SCROLLABLE);
    s_msg_title = lv_label_create(s_msg_panel);
    lv_label_set_long_mode(s_msg_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg_title, SCREEN_W - 40);
    lv_obj_set_style_text_font(s_msg_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_msg_title, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_msg_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg_title, LV_ALIGN_CENTER, 0, -30);
    s_msg_detail = lv_label_create(s_msg_panel);
    lv_label_set_long_mode(s_msg_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg_detail, SCREEN_W - 40);
    lv_obj_set_style_text_font(s_msg_detail, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_msg_detail, lv_color_hex(0x99aabb), 0);
    lv_obj_set_style_text_align(s_msg_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg_detail, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
    ESP_LOGI(TAG, "screen built: %dx%d, text only", SCREEN_W, SCREEN_H);
}

void display_set_state(display_state_t st)
{
    static const char *names[] = {"listening", "hearing you", "working", "problem"};
    static const uint32_t colours[] = {0x8899aa, 0x44dd88, 0xffcc44, 0xff5555};
    if (s_chip == NULL || st > DISPLAY_ERROR) {
        return;
    }
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_chip, names[st]);
        lv_obj_set_style_text_color(s_chip, lv_color_hex(colours[st]), 0);
        lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, 10);
        bsp_display_unlock();
    }
}

void display_append(const char *text, float confidence)
{
    if (s_text == NULL || text == NULL || *text == '\0') {
        return;
    }
    size_t add = strlen(text) + 1;
    /* Drop from the front, whole lines at a time, rather than truncating the tail. */
    while (s_len + add >= TRANSCRIPT_MAX) {
        char *nl = memchr(s_buf, '\n', s_len);
        if (nl == NULL) {
            s_len = 0;
            break;
        }
        size_t drop = (size_t)(nl - s_buf) + 1;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
    }
    if (s_len > 0 && s_len < TRANSCRIPT_MAX - 1) {
        s_buf[s_len++] = '\n';
    }
    size_t n = strlen(text);
    if (n > TRANSCRIPT_MAX - 1 - s_len) {
        n = TRANSCRIPT_MAX - 1 - s_len;
    }
    memcpy(s_buf + s_len, text, n);
    s_len += n;
    s_buf[s_len] = '\0';

    if (bsp_display_lock(200)) {
        lv_label_set_text(s_text, s_buf);
        lv_obj_scroll_to_view(s_text, LV_ANIM_OFF);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "transcript += '%s' (%.2f)", text, confidence);
}

void display_detail(const char *text)
{
    if (s_detail == NULL) {
        return;
    }
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_detail, text ? text : "");
        lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -12);
        bsp_display_unlock();
    }
}

void display_message(const char *title, const char *detail)
{
    if (s_msg_panel == NULL) {
        return;
    }
    if (bsp_display_lock(300)) {
        if (title == NULL) {
            lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_msg_title, title);
            lv_label_set_text(s_msg_detail, detail ? detail : "");
            lv_obj_align(s_msg_title, LV_ALIGN_CENTER, 0, -30);
            lv_obj_align(s_msg_detail, LV_ALIGN_CENTER, 0, 30);
            lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_msg_panel);
        }
        bsp_display_unlock();
    }
}

void display_clear(void)
{
    s_len = 0;
    s_buf[0] = '\0';
    if (s_text != NULL && bsp_display_lock(200)) {
        lv_label_set_text(s_text, "");
        bsp_display_unlock();
    }
}
