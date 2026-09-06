/*
 * The BOOT button on GPIO 0, sampled every 10 ms from a timer. GPIO 0 is a strapping
 * pin with its own external pull-up on this board; we only read it, never reconfigure
 * its pulls or attach an interrupt. The main loop turns every 250 ms and a quick human
 * press is shorter than that (2026-09-06: presses of 100-200 ms could fall between two
 * polls and vanish without a trace), so the timer latches each press and the loop
 * collects it at its own pace.
 */

#include "button.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define BUTTON_GPIO      GPIO_NUM_0
#define SAMPLE_US        (10 * 1000)
#define DEBOUNCE_SAMPLES 2 /* 20 ms of agreement before a change counts */
#define LONG_PRESS_US    ((int64_t)CONFIG_DICT_LONG_PRESS_MS * 1000)

static const char *TAG = "button";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_down; /* debounced */
static int s_disagree;
static int64_t s_down_since;
static bool s_long_fired;
static button_event_t s_pending; /* latched for the main loop; a newer press replaces an uncollected one */
static uint32_t s_pending_held_ms, s_last_held_ms, s_dropped;

static void latch(button_event_t ev, uint32_t held_ms)
{
    portENTER_CRITICAL(&s_lock);
    if (s_pending != BUTTON_NONE) {
        s_dropped++;
    }
    s_pending = ev;
    s_pending_held_ms = held_ms;
    portEXIT_CRITICAL(&s_lock);
}

static void sample(void *arg)
{
    (void)arg;
    bool raw_down = gpio_get_level(BUTTON_GPIO) == 0;
    if (raw_down == s_down) {
        s_disagree = 0;
    } else if (++s_disagree >= DEBOUNCE_SAMPLES) {
        s_disagree = 0;
        s_down = raw_down;
        int64_t now = esp_timer_get_time();
        if (s_down) {
            s_down_since = now;
            s_long_fired = false;
        } else if (!s_long_fired) {
            latch(BUTTON_SHORT, (uint32_t)((now - s_down_since) / 1000));
        }
        return;
    }
    if (s_down && !s_long_fired && esp_timer_get_time() - s_down_since >= LONG_PRESS_US) {
        s_long_fired = true;
        latch(BUTTON_LONG, (uint32_t)CONFIG_DICT_LONG_PRESS_MS);
    }
}

void button_init(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    s_down = gpio_get_level(BUTTON_GPIO) == 0;
    const esp_timer_create_args_t args = {.callback = sample, .name = "button", .dispatch_method = ESP_TIMER_TASK};
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, SAMPLE_US));
    ESP_LOGI(TAG, "BOOT button on GPIO %d, sampled every %d ms, long press %d ms; level now %d", BUTTON_GPIO,
             SAMPLE_US / 1000, CONFIG_DICT_LONG_PRESS_MS, gpio_get_level(BUTTON_GPIO));
}

button_event_t button_poll(void)
{
    portENTER_CRITICAL(&s_lock);
    button_event_t ev = s_pending;
    s_pending = BUTTON_NONE;
    if (ev != BUTTON_NONE) {
        s_last_held_ms = s_pending_held_ms;
    }
    uint32_t dropped = s_dropped;
    s_dropped = 0;
    portEXIT_CRITICAL(&s_lock);
    if (dropped) {
        ESP_LOGW(TAG, "%" PRIu32 " press(es) came faster than the main loop and were not collected", dropped);
    }
    return ev;
}

uint32_t button_last_held_ms(void)
{
    return s_last_held_ms;
}
