#include "dbmap_wifi.h"

#include <string.h>

#include "dbmap_backoff.h"
#include "dbmap_diag.h"
#include "dbmap_lifecycle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"

static const char *TAG = "dbmap_wifi";
static EventGroupHandle_t s_wifi;
#define GOT_IP BIT0

static dbmap_backoff_t s_backoff;
static esp_timer_handle_t s_reconnect_timer;
static bool s_reconnect_armed;
static bool s_disconnect_counted;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t wifi_rand32(void)
{
    return esp_random();
}

static void wifi_reconnect_timer_cb(void *arg);

static void cancel_reconnect_timer(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
    s_reconnect_armed = false;
}

static void schedule_wifi_reconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (s_reconnect_armed || dbmap_backoff_is_in_flight(&s_backoff)) {
        return;
    }
    uint32_t delay = dbmap_backoff_delay_ms(&s_backoff, wifi_rand32);
    int64_t ready_in = s_backoff.next_attempt_at_ms - now_ms();
    if (ready_in > 0 && (uint32_t)ready_in < delay) {
        delay = (uint32_t)ready_in;
    }
    if (delay < 1) {
        delay = 1;
    }

    cancel_reconnect_timer();
    const esp_timer_create_args_t args = {
        .callback = &wifi_reconnect_timer_cb,
        .name = "wifi_bo",
    };
    if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
        return;
    }
    if (esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000ULL) != ESP_OK) {
        cancel_reconnect_timer();
        return;
    }
    s_reconnect_armed = true;
    ESP_LOGI(TAG, "scheduling Wi-Fi reconnect in %lu ms (failures=%lu)",
             (unsigned long)delay,
             (unsigned long)s_backoff.consecutive_failures);
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    s_reconnect_armed = false;
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (dbmap_wifi_has_ip()) {
        return;
    }
    if (dbmap_backoff_is_in_flight(&s_backoff)) {
        return;
    }
    dbmap_backoff_set_in_flight(&s_backoff, true);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconnect failed: %s", esp_err_to_name(err));
        dbmap_backoff_note_failure(&s_backoff, now_ms());
        dbmap_backoff_set_in_flight(&s_backoff, false);
        schedule_wifi_reconnect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *event = data;
    ESP_LOGI(TAG, "got IPv4 address " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi, GOT_IP);
    s_disconnect_counted = false;
    cancel_reconnect_timer();
    dbmap_backoff_note_success(&s_backoff, now_ms());
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "disconnected from access point (reason=%d)", event->reason);
        xEventGroupClearBits(s_wifi, GOT_IP);
        dbmap_backoff_set_in_flight(&s_backoff, false);
        if (dbmap_lifecycle_ota_reboot_pending()) {
            ESP_LOGI(TAG, "Wi-Fi disconnect during OTA reboot pending (expected)");
            if (!s_disconnect_counted) {
                s_disconnect_counted = true;
                dbmap_diag_note_wifi_disconnect();
            }
            return;
        }
        if (!s_disconnect_counted) {
            s_disconnect_counted = true;
            dbmap_diag_note_wifi_disconnect();
            dbmap_backoff_note_failure(&s_backoff, now_ms());
        }
        schedule_wifi_reconnect();
    }
}

bool dbmap_wifi_has_ip(void)
{
    if (s_wifi == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi) & GOT_IP) != 0;
}

esp_err_t dbmap_wifi_start(const dbmap_network_t *net)
{
    dbmap_backoff_config_t bo_cfg;
    dbmap_backoff_config_wifi(&bo_cfg);
    dbmap_backoff_init(&s_backoff, &bo_cfg);

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
    dbmap_backoff_set_in_flight(&s_backoff, true);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if ((bits & GOT_IP) == 0) {
        ESP_LOGE(TAG, "Wi-Fi timeout");
        dbmap_backoff_set_in_flight(&s_backoff, false);
        dbmap_backoff_note_failure(&s_backoff, now_ms());
        schedule_wifi_reconnect();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Wi-Fi up");

    /* Modem sleep commonly stalls MQTT/TLS keepalive writes on ESP32 station links. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for stable MQTT");
    } else {
        ESP_LOGW(TAG, "could not disable Wi-Fi power save: %s", esp_err_to_name(ps_err));
    }

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        ESP_LOGI(TAG, "mDNS client ready for *.local lookup");
    } else if (mdns_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "mDNS client already running");
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s; *.local names may not resolve", esp_err_to_name(mdns_err));
    }
    return ESP_OK;
}

esp_err_t dbmap_wifi_get_rssi(int *rssi_dbm)
{
    if (rssi_dbm == NULL) return ESP_ERR_INVALID_ARG;
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK) *rssi_dbm = ap_info.rssi;
    return err;
}

void dbmap_wifi_prepare_ota_reboot(void)
{
    cancel_reconnect_timer();
    dbmap_backoff_set_in_flight(&s_backoff, false);
}
