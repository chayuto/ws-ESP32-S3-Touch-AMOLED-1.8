/*
 * The BOOT button on GPIO 0, polled. GPIO 0 is a strapping pin with its own
 * external pull-up on this board; we only read it, never reconfigure its pulls
 * or attach an interrupt. A human press lasts far longer than one main-loop
 * turn (250 ms), so polling loses nothing.
 */

#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"

#define BUTTON_GPIO GPIO_NUM_0

static const char *TAG = "button";
static bool s_was_down;

void button_init(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    s_was_down = gpio_get_level(BUTTON_GPIO) == 0;
    ESP_LOGI(TAG, "BOOT button on GPIO %d, polled; level now %d", BUTTON_GPIO, gpio_get_level(BUTTON_GPIO));
}

bool button_pressed(void)
{
    bool down = gpio_get_level(BUTTON_GPIO) == 0;
    bool edge = down && !s_was_down;
    s_was_down = down;
    return edge;
}
