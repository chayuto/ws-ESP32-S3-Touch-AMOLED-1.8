#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";
static int s_join_ms;

static EventGroupHandle_t s_ev;
static esp_netif_t *s_netif;
static bool s_up, s_stack_inited;
static char s_ip[16];
#define GOT_IP BIT0
#define FAILED BIT1

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        xEventGroupSetBits(s_ev, FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, GOT_IP);
    }
}

bool wifi_sta_join(int timeout_s)
{
    if (strlen(CONFIG_DICT_WIFI_SSID) == 0) {
        ESP_LOGI(TAG, "no network configured");
        return false;
    }
    if (s_up) {
        return true;
    }
    if (!s_stack_inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_ev = xEventGroupCreate();
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, evt, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, evt, NULL);
        s_stack_inited = true;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_DICT_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_DICT_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    xEventGroupClearBits(s_ev, GOT_IP | FAILED);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining '%s' (up to %d s)...", CONFIG_DICT_WIFI_SSID, timeout_s);
    int64_t t0 = esp_timer_get_time();
    EventBits_t bits = xEventGroupWaitBits(s_ev, GOT_IP | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_s * 1000));
    s_join_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (bits & GOT_IP) {
        s_up = true;
        esp_wifi_set_ps(WIFI_PS_NONE); /* we serve requests; a dozing station drops them */
        int rssi = 0, chan = 0;
        wifi_sta_signal(&rssi, &chan);
        ESP_LOGI(TAG, "joined in %d ms, ip %s, rssi %d dBm, channel %d", s_join_ms, s_ip, rssi, chan);
        return true;
    }
    ESP_LOGW(TAG, "'%s' not joined (%s)", CONFIG_DICT_WIFI_SSID, (bits & FAILED) ? "rejected or not found" : "timeout");
    wifi_sta_leave();
    return false;
}

bool wifi_sta_connected(char *ip, int len)
{
    if (ip) {
        strlcpy(ip, s_ip, len);
    }
    return s_up && s_ip[0] != '\0';
}

void wifi_sta_leave(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_up = false;
    s_ip[0] = '\0';
    ESP_LOGI(TAG, "off");
}

bool wifi_sta_signal(int *rssi, int *channel)
{
    wifi_ap_record_t ap;
    if (!s_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    if (rssi) *rssi = ap.rssi;
    if (channel) *channel = ap.primary;
    return true;
}

int wifi_sta_join_ms(void)
{
    return s_join_ms;
}
