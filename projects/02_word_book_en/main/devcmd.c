#include "devcmd.h"

#include <stdio.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "devcmd";
static volatile char s_pending;

static void reader(void *arg)
{
    (void)arg;
    uint8_t c;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(500));
        if (n == 1 && c > ' ' && c < 127) {
            ESP_LOGI(TAG, "command '%c'", c);
            s_pending = (char)c;
        }
    }
}

void devcmd_init(void)
{
    /* The console is already routed to USB-Serial-JTAG; install the driver so we can read it. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "driver install: %s; commands off", esp_err_to_name(err));
        return;
    }
    xTaskCreatePinnedToCore(reader, "devcmd", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "listening: m=maintenance r=reload s=sleep i=info d=debug-all p=power b=bright x=panel-reinit");
}

char devcmd_take(void)
{
    char c = s_pending;
    s_pending = 0;
    return c;
}
