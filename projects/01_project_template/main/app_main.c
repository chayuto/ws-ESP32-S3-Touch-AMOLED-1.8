/*
 * 01_project_template — the starting point for projects on the
 * Waveshare ESP32-S3-Touch-AMOLED-1.8.
 *
 * Copy this directory, rename the project in the top-level CMakeLists.txt, and
 * replace build_ui(). Everything else is board-correct as it stands.
 *
 * It brings up the full display stack (CO5300 QSPI panel + CST820 touch + LVGL 9)
 * and draws a screen you can tap. Every touch is also logged to the serial console,
 * so the template is verifiable headlessly — you do not need to see the panel to
 * know the stack works.
 */

#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#define BRIGHTNESS_PERCENT 85

static lv_obj_t *s_status_label;
static uint32_t s_tap_count;

/* Tap handler. Runs in the LVGL task, which already holds the port lock. */
static void screen_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = {0, 0};
    if (indev != NULL) {
        lv_indev_get_point(indev, &p);
    }

    s_tap_count++;
    lv_label_set_text_fmt(s_status_label, "taps: %" PRIu32 "\nlast: %d, %d",
                          s_tap_count, (int)p.x, (int)p.y);

    /* Mirror to serial so touch is verifiable without looking at the panel. */
    ESP_LOGI(TAG, "touch #%" PRIu32 " at x=%d y=%d", s_tap_count, (int)p.x, (int)p.y);
}

/* Replace this with your own UI. Caller must hold the LVGL port lock. */
static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-S3-Touch-AMOLED-1.8");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0F0F5), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text_fmt(subtitle, "%dx%d  -  project template", BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8A8A99), LV_PART_MAIN);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "tap anywhere");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A9BF5), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -20);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "taps: 0\nlast: -, -");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xF0F0F5), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 60);

    lv_obj_add_event_cb(scr, screen_pressed_cb, LV_EVENT_PRESSED, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "01_project_template starting");

    /* bsp_i2c_init() logs a pull-up warning on this board even though the
     * hardware pull-ups are fine and all six I2C devices enumerate. Mute it
     * across BSP bring-up so a real error is not lost in the noise. */
    /* Own tag at DEBUG from the first line (see .claude/skills/agentic-logging). The BSP's
     * i2c.master pull-up warning during bring-up is benign here and stays visible on purpose. */
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    /* Panel + touch + LVGL port, in one call. Draw buffers land in PSRAM. */
    lv_display_t *display = bsp_display_start();

    if (display == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(BRIGHTNESS_PERCENT));

    ESP_LOGI(TAG, "display up: %dx%d, touch indev %s",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
             bsp_display_get_input_dev() != NULL ? "registered" : "MISSING");

    /* Any LVGL call from outside the LVGL task must hold this lock. */
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return;
    }
    build_ui();
    bsp_display_unlock();

    ESP_LOGI(TAG, "UI drawn; tap the panel to see touch events here");

    while (true) {
        ESP_LOGI(TAG, "alive: taps=%" PRIu32 " internal=%u psram=%u",
                 s_tap_count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
