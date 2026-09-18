/*
 * The status screen. 368x448, one page, everything the board is measuring.
 *
 * The panel control at the top of this file is carried over from
 * 02_word_book_en/cards.c by way of 05_dictation, verbatim, comments included,
 * because both of those comments describe a fault that cost a day. Do not
 * simplify them away.
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

static lv_obj_t *s_mode;      /* the mode chip, top left */
static lv_obj_t *s_clock;     /* wall clock, top right */
static lv_obj_t *s_power;     /* battery, charge state, board temperature */
static lv_obj_t *s_wifi_big;  /* the two numbers that matter: addresses seen */
static lv_obj_t *s_ble_big;
static lv_obj_t *s_wifi_sub;
static lv_obj_t *s_ble_sub;
static lv_obj_t *s_imu;
static lv_obj_t *s_sound;
static lv_obj_t *s_card;
static lv_obj_t *s_note;      /* what it is doing right now */
static lv_obj_t *s_card_btn;
static lv_obj_t *s_card_btn_label;
static volatile bool s_card_tap;
static lv_obj_t *s_msg_panel; /* full-screen overlay */
static lv_obj_t *s_msg_title;
static lv_obj_t *s_msg_detail;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;


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

/* Snap every invalidated area to the 2x2 grid the panel addresses in. */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

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

    /* The CO5300 takes a window in pairs of pixels: a flush whose area starts on
     * an odd column or carries an odd width lands in a window one pixel from the
     * data, and every row after the first is drawn one further across - text
     * comes out sheared, with the pixels it should have replaced still on the
     * glass. The BSP knows this and attaches this rounder, but only inside
     * bsp_display_start(), which this project does not call: that function
     * allocates LVGL's draw buffer with MALLOC_CAP_DEFAULT and the buffer lands
     * in PSRAM under Wi-Fi's RAM pressure, which breaks every SPI flush instead.
     * So the display is built by hand here, and the rounder has to come with it.
     * Reported from the glass on 2026-09-18: tilted and overwritten text.
     * Copied from the BSP's rounder_event_cb() so the two cannot drift. */
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
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


/* ------------------------------------------------------------------------- */
/* The screen                                                                */
/* ------------------------------------------------------------------------- */

/* Latched, not acted on here: the LVGL task must never touch the card. */
static void card_btn_cb(lv_event_t *e)
{
    (void)e;
    s_card_tap = true;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour, const char *initial)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_label_set_text(l, initial);
    return l;
}

/* A bordered block with a heading, used for the two radio panels. */
static lv_obj_t *make_panel(lv_obj_t *parent, int w, int h, const char *heading, lv_color_t accent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x111318), 0);
    lv_obj_set_style_border_color(p, accent, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 8, 0);
    lv_obj_set_style_pad_all(p, 8, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(p, 0, 0);
    make_label(p, &lv_font_montserrat_14, accent, heading);
    return p;
}

void display_init(void)
{
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "no display lock; screen not built");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- top row: mode and clock ------------------------------------- */
    s_mode = make_label(scr, &lv_font_montserrat_20, lv_color_hex(0x6fd3ff), "starting");
    lv_obj_align(s_mode, LV_ALIGN_TOP_LEFT, 0, 0);

    s_clock = make_label(scr, &lv_font_montserrat_16, lv_color_hex(0x9aa0a6), "--:--:--");
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_power = make_label(scr, &lv_font_montserrat_14, lv_color_hex(0x9aa0a6), "power ?");
    lv_obj_align(s_power, LV_ALIGN_TOP_LEFT, 0, 30);

    /* --- the two radio panels, side by side --------------------------- */
    const int panel_w = (SCREEN_W - 20 - 10) / 2; /* screen minus padding minus the gap */
    lv_obj_t *wifi = make_panel(scr, panel_w, 104, "WI-FI", lv_color_hex(0x4da3ff));
    lv_obj_align(wifi, LV_ALIGN_TOP_LEFT, 0, 56);
    s_wifi_big = make_label(wifi, &lv_font_montserrat_28, lv_color_white(), "0");
    s_wifi_sub = make_label(wifi, &lv_font_montserrat_14, lv_color_hex(0x9aa0a6), "-- now");

    lv_obj_t *ble = make_panel(scr, panel_w, 104, "BLUETOOTH", lv_color_hex(0xb388ff));
    lv_obj_align(ble, LV_ALIGN_TOP_RIGHT, 0, 56);
    s_ble_big = make_label(ble, &lv_font_montserrat_28, lv_color_white(), "0");
    s_ble_sub = make_label(ble, &lv_font_montserrat_14, lv_color_hex(0x9aa0a6), "-- now");

    /* --- motion and storage ------------------------------------------- */
    s_imu = make_label(scr, &lv_font_montserrat_16, lv_color_hex(0xd0d4d8), "IMU --");
    lv_obj_align(s_imu, LV_ALIGN_TOP_LEFT, 0, 174);

    s_sound = make_label(scr, &lv_font_montserrat_16, lv_color_hex(0xd0d4d8), "sound --");
    lv_obj_align(s_sound, LV_ALIGN_TOP_LEFT, 0, 202);

    s_card = make_label(scr, &lv_font_montserrat_16, lv_color_hex(0xd0d4d8), "card --");
    lv_obj_align(s_card, LV_ALIGN_TOP_LEFT, 0, 230);

    /* --- the running commentary, at the bottom where a person looks --- */
    s_note = make_label(scr, &lv_font_montserrat_16, lv_color_hex(0x6fd3ff), "starting up");
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_note, SCREEN_W - 20);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* --- the card button, so the card can be pulled without a laptop ---- */
    s_card_btn = lv_button_create(scr);
    lv_obj_set_size(s_card_btn, 130, 44);
    lv_obj_align(s_card_btn, LV_ALIGN_BOTTOM_RIGHT, 0, -40);
    lv_obj_set_style_bg_color(s_card_btn, lv_color_hex(0x2a3038), 0);
    lv_obj_set_style_border_color(s_card_btn, lv_color_hex(0x4da3ff), 0);
    lv_obj_set_style_border_width(s_card_btn, 1, 0);
    lv_obj_set_style_radius(s_card_btn, 10, 0);
    lv_obj_add_event_cb(s_card_btn, card_btn_cb, LV_EVENT_CLICKED, NULL);
    s_card_btn_label = make_label(s_card_btn, &lv_font_montserrat_16, lv_color_white(), "EJECT");
    lv_obj_center(s_card_btn_label);

    /* --- the overlay, hidden until something needs the whole screen --- */
    s_msg_panel = lv_obj_create(scr);
    lv_obj_set_size(s_msg_panel, SCREEN_W, SCREEN_H);
    lv_obj_center(s_msg_panel);
    lv_obj_set_style_bg_color(s_msg_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_msg_panel, 0, 0);
    lv_obj_set_style_radius(s_msg_panel, 0, 0);
    lv_obj_clear_flag(s_msg_panel, LV_OBJ_FLAG_SCROLLABLE);
    s_msg_title = make_label(s_msg_panel, &lv_font_montserrat_28, lv_color_white(), "");
    lv_obj_align(s_msg_title, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_text_align(s_msg_title, LV_TEXT_ALIGN_CENTER, 0);
    s_msg_detail = make_label(s_msg_panel, &lv_font_montserrat_16, lv_color_hex(0x9aa0a6), "");
    lv_label_set_long_mode(s_msg_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg_detail, SCREEN_W - 60);
    lv_obj_set_style_text_align(s_msg_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg_detail, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
    ESP_LOGI(TAG, "screen built");
}

void display_update(const display_status_t *st)
{
    if (s_mode == NULL || !bsp_display_lock(200)) {
        return;
    }

    lv_label_set_text(s_mode, st->mode ? st->mode : "?");
    lv_label_set_text(s_clock, st->clock ? st->clock : "unset");

    /* Every line with a float in it is formatted with the C library's snprintf
     * and handed over as a finished string. LVGL's own printf is the builtin
     * one (CONFIG_LV_USE_BUILTIN_SPRINTF=y) and it is compiled without float
     * support (CONFIG_LV_USE_FLOAT is not set), so lv_label_set_text_fmt()
     * does not consume the double for a %f - it prints the letter and leaves
     * the argument on the list. The next %s then reads the float's bytes as a
     * pointer. That is a LoadProhibited panic at EXCVADDR 0xa0000000, and it
     * killed the first boot of this project on 2026-09-18, in this function,
     * on the line below. Do not put a %f back into an LVGL format string. */
    char line[96];

    /* One line for power, because on a battery device it is the number that
     * decides whether the run finishes. */
    snprintf(line, sizeof line, "%d%% %s   %.0f C%s%s", st->batt_pct,
             st->charging ? "charging" : (st->vbus ? "on USB" : "on battery"), (double)st->board_c,
             st->ip ? "   " : "", st->ip ? st->ip : "");
    lv_label_set_text(s_power, line);

    lv_label_set_text_fmt(s_wifi_big, "%u", (unsigned)st->wifi_known);
    if (st->wifi_last >= 0) {
        lv_label_set_text_fmt(s_wifi_sub, "%d now", st->wifi_last);
    } else {
        lv_label_set_text(s_wifi_sub, "-- now");
    }
    lv_label_set_text_fmt(s_ble_big, "%u", (unsigned)st->ble_known);
    if (st->ble_last >= 0) {
        lv_label_set_text_fmt(s_ble_sub, "%d now", st->ble_last);
    } else {
        lv_label_set_text(s_ble_sub, "-- now");
    }

    if (st->imu_ok) {
        snprintf(line, sizeof line, "tilt %+.0f / %+.0f   motion %.2f", (double)st->imu_pitch,
                 (double)st->imu_roll, (double)st->imu_dyn);
        lv_label_set_text(s_imu, line);
    } else {
        lv_label_set_text(s_imu, "IMU absent");
    }

    /* dBFS, so the numbers are negative and quiet is more negative. Saying the
     * unit on the glass stops anyone reading it as decibels SPL. */
    if (st->sound_ok) {
        snprintf(line, sizeof line, "sound %.0f dBFS", (double)st->sound_dbfs);
        lv_label_set_text(s_sound, line);
    } else {
        lv_label_set_text(s_sound, "sound --");
    }

    /* The card line says the truth even when the truth is "no card". A logger
     * that quietly stops logging is the failure mode this whole project is about. */
    if (st->card_ejected) {
        lv_obj_set_style_text_color(s_card, lv_color_hex(0xffd166), 0);
        lv_label_set_text(s_card, "card ejected - safe to remove");
    } else if (!st->card) {
        lv_obj_set_style_text_color(s_card, lv_color_hex(0xff6b6b), 0);
        lv_label_set_text(s_card, "NO CARD - nothing is being saved");
    } else {
        lv_obj_set_style_text_color(s_card, lv_color_hex(0xd0d4d8), 0);
        lv_label_set_text_fmt(s_card, "card %u MB free   %u records   %u scans", (unsigned)st->card_free_mb,
                              (unsigned)st->records, (unsigned)st->scans);
    }

    if (st->note) {
        lv_label_set_text(s_note, st->note);
    }

    lv_label_set_text(s_card_btn_label, (st->card_ejected || !st->card) ? "MOUNT" : "EJECT");

    bsp_display_unlock();
}

void display_message(const char *title, const char *detail)
{
    if (s_msg_panel == NULL || !bsp_display_lock(300)) {
        return;
    }
    lv_label_set_text(s_msg_title, title ? title : "");
    lv_label_set_text(s_msg_detail, detail ? detail : "");
    lv_obj_align(s_msg_title, LV_ALIGN_CENTER, 0, -40);
    lv_obj_align(s_msg_detail, LV_ALIGN_CENTER, 0, 20);
    lv_obj_clear_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_msg_panel);
    bsp_display_unlock();
    ESP_LOGI(TAG, "screen: %s | %s", title ? title : "", detail ? detail : "");
}

bool display_take_card_tap(void)
{
    bool v = s_card_tap;
    s_card_tap = false;
    return v;
}

void display_clear_message(void)
{
    if (s_msg_panel == NULL || !bsp_display_lock(300)) {
        return;
    }
    lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}
