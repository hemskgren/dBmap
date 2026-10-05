#include "dbmap_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "dbmap_wifi";
static EventGroupHandle_t s_wifi;
#define GOT_IP BIT0

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    xEventGroupSetBits(s_wifi, GOT_IP);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "disconnected from access point (reason=%d)", event->reason);
        xEventGroupClearBits(s_wifi, GOT_IP);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reconnect failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t dbmap_wifi_start(const dbmap_network_t *net)
{
    s_wifi = xEventGroupCreate();
    if (s_wifi == NULL) {
        ESP_LOGE(TAG, "failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, net->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, net->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "connecting to configured SSID '%s'", net->ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if ((bits & GOT_IP) == 0) {
        ESP_LOGE(TAG, "Wi-Fi timeout");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Wi-Fi up");
    return ESP_OK;
}
