/*
 * The BOOT button on GPIO 0, polled. GPIO 0 is a strapping pin with its own
 * external pull-up on this board; we only read it, never reconfigure its pulls
 * or attach an interrupt. A human press lasts far longer than one main-loop
 * turn (250 ms), so polling loses nothing.
 */

#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define BUTTON_GPIO   GPIO_NUM_0
#define LONG_PRESS_US (1500 * 1000)

static const char *TAG = "button";
static bool s_was_down;
static int64_t s_down_since;
static bool s_long_fired;

void button_init(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    s_was_down = gpio_get_level(BUTTON_GPIO) == 0;
    ESP_LOGI(TAG, "BOOT button on GPIO %d, polled; level now %d", BUTTON_GPIO, gpio_get_level(BUTTON_GPIO));
}

button_event_t button_poll(void)
{
    bool down = gpio_get_level(BUTTON_GPIO) == 0;
    button_event_t ev = BUTTON_NONE;
    int64_t now = esp_timer_get_time();
    if (down && !s_was_down) {
        s_down_since = now;
        s_long_fired = false;
    } else if (down && !s_long_fired && now - s_down_since >= LONG_PRESS_US) {
        s_long_fired = true;
        ev = BUTTON_LONG;
    } else if (!down && s_was_down && !s_long_fired) {
        ev = BUTTON_SHORT;
    }
    s_was_down = down;
    return ev;
}
