#include "sp_wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sp_backoff.h"
#include "sp_diag.h"
#include "sp_lifecycle.h"
#include "sp_wifi_reconnect_sm.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "mdns.h"

static const char *TAG = "sp_wifi";
static EventGroupHandle_t s_wifi;
#define GOT_IP BIT0

static sp_backoff_t s_backoff;
static sp_wifi_reconnect_sm_t s_sm;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_watchdog_timer;
static uint32_t s_watchdog_timer_generation;
static esp_netif_t *s_sta_netif;

/**
 * Serializes SM mutations across the Wi-Fi event task and esp_timer task.
 * Never hold across timer create/delete, Wi-Fi driver calls, or logging.
 */
static portMUX_TYPE s_wifi_sm_lock = portMUX_INITIALIZER_UNLOCKED;

/** Rate-limit STA_DISCONNECTED logs from the Wi-Fi event task (microseconds). */
static int64_t s_disconnect_log_us;
static uint32_t s_disconnect_log_suppressed;
static int s_disconnect_log_reason = -1;

#define WIFI_DISCONNECT_LOG_INTERVAL_US (30000000LL)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case 1:
        return "UNSPECIFIED";
    case 2:
        return "AUTH_EXPIRE";
    case 3:
        return "AUTH_LEAVE";
    case 4:
        return "ASSOC_EXPIRE";
    case 8:
        return "ASSOC_LEAVE";
    case 15:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case 200:
        return "BEACON_TIMEOUT"; /* older IDF alias; 205 used on current trees */
    case 201:
        return "NO_AP_FOUND";
    case 202:
        return "AUTH_FAIL";
    case 203:
        return "ASSOC_FAIL";
    case 204:
        return "HANDSHAKE_TIMEOUT";
    case 205:
        return "BEACON_TIMEOUT";
    case 210:
        return "CONNECTION_FAIL";
    default:
        return "UNKNOWN";
    }
}

static bool sta_netif_has_ip(void)
{
    if (s_sta_netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) {
        return false;
    }
    return info.ip.addr != 0;
}

static void append_ap_info_diag(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    buf[0] = '\0';
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        snprintf(buf, buflen, " ap_info=unavailable");
        return;
    }
    snprintf(
        buf,
        buflen,
        " rssi=%d channel=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
        ap.rssi,
        (unsigned)ap.primary,
        ap.bssid[0],
        ap.bssid[1],
        ap.bssid[2],
        ap.bssid[3],
        ap.bssid[4],
        ap.bssid[5]
    );
}

static void log_sta_disconnected_rate_limited(int reason)
{
    const int64_t now_us = esp_timer_get_time();
    const bool reason_changed = (reason != s_disconnect_log_reason);
    if (s_disconnect_log_us == 0
        || reason_changed
        || (now_us - s_disconnect_log_us) >= WIFI_DISCONNECT_LOG_INTERVAL_US) {
        char ap_diag[96];
        append_ap_info_diag(ap_diag, sizeof(ap_diag));
        if (s_disconnect_log_suppressed > 0) {
            ESP_LOGW(
                TAG,
                "disconnected from access point (reason=%d/%s failures=%lu "
                "armed=%d in_flight=%d netif_ip=%d); suppressed %lu duplicate log(s)%s",
                reason,
                wifi_disconnect_reason_name((uint8_t)reason),
                (unsigned long)s_backoff.consecutive_failures,
                s_sm.reconnect_armed ? 1 : 0,
                s_sm.in_flight ? 1 : 0,
                sta_netif_has_ip() ? 1 : 0,
                (unsigned long)s_disconnect_log_suppressed,
                ap_diag
            );
            s_disconnect_log_suppressed = 0;
        } else {
            ESP_LOGW(
                TAG,
                "disconnected from access point (reason=%d/%s failures=%lu "
                "armed=%d in_flight=%d netif_ip=%d)%s",
                reason,
                wifi_disconnect_reason_name((uint8_t)reason),
                (unsigned long)s_backoff.consecutive_failures,
                s_sm.reconnect_armed ? 1 : 0,
                s_sm.in_flight ? 1 : 0,
                sta_netif_has_ip() ? 1 : 0,
                ap_diag
            );
        }
        s_disconnect_log_us = now_us;
        s_disconnect_log_reason = reason;
    } else {
        s_disconnect_log_suppressed++;
    }
}

static uint32_t wifi_rand32(void)
{
    return esp_random();
}

static void wifi_reconnect_timer_cb(void *arg);
static void wifi_in_flight_watchdog_cb(void *arg);

/** Delete reconnect timer handle only; SM owns reconnect_armed. */
static void delete_reconnect_timer_handle(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
}

static void cancel_watchdog_timer(void)
{
    if (s_watchdog_timer == NULL) {
        return;
    }
    esp_timer_stop(s_watchdog_timer);
    esp_timer_delete(s_watchdog_timer);
    s_watchdog_timer = NULL;
    s_watchdog_timer_generation = 0;
}

static void arm_watchdog_timer(uint32_t generation)
{
    cancel_watchdog_timer();
    const esp_timer_create_args_t args = {
        .callback = &wifi_in_flight_watchdog_cb,
        .arg = (void *)(uintptr_t)generation,
        .name = "wifi_if",
    };
    if (esp_timer_create(&args, &s_watchdog_timer) != ESP_OK) {
        ESP_LOGW(TAG, "failed to create in-flight Wi-Fi watchdog timer");
        return;
    }
    if (esp_timer_start_once(
            s_watchdog_timer,
            (uint64_t)s_sm.in_flight_timeout_ms * 1000ULL
        )
        != ESP_OK) {
        cancel_watchdog_timer();
        ESP_LOGW(TAG, "failed to start in-flight Wi-Fi watchdog timer");
        return;
    }
    s_watchdog_timer_generation = generation;
}

static void apply_sm_result(const sp_wifi_sm_result_t *result);

/**
 * Create the oneshot for an SM-held reconnect reservation.
 * Must not re-check can_schedule — reservation is already exclusive.
 */
static void arm_reserved_reconnect_timer(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        portENTER_CRITICAL(&s_wifi_sm_lock);
        sp_wifi_reconnect_sm_release_reconnect_reservation(&s_sm);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        return;
    }

    uint32_t delay = sp_backoff_delay_ms(&s_backoff, wifi_rand32);
    int64_t ready_in = s_backoff.next_attempt_at_ms - now_ms();
    if (ready_in > 0 && (uint32_t)ready_in < delay) {
        delay = (uint32_t)ready_in;
    }
    if (delay < 1) {
        delay = 1;
    }

    delete_reconnect_timer_handle();
    const esp_timer_create_args_t args = {
        .callback = &wifi_reconnect_timer_cb,
        .name = "wifi_bo",
    };
    if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
        portENTER_CRITICAL(&s_wifi_sm_lock);
        sp_wifi_reconnect_sm_release_reconnect_reservation(&s_sm);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        return;
    }
    if (esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000ULL) != ESP_OK) {
        delete_reconnect_timer_handle();
        portENTER_CRITICAL(&s_wifi_sm_lock);
        sp_wifi_reconnect_sm_release_reconnect_reservation(&s_sm);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        return;
    }
    ESP_LOGI(TAG, "scheduling Wi-Fi reconnect in %lu ms (failures=%lu)",
             (unsigned long)delay,
             (unsigned long)s_backoff.consecutive_failures);
}

static void apply_sm_result(const sp_wifi_sm_result_t *result)
{
    if (result == NULL) {
        return;
    }
    if (result->cancel_reconnect_timer) {
        delete_reconnect_timer_handle();
    }
    if (result->cancel_watchdog) {
        cancel_watchdog_timer();
    }
    if (result->arm_watchdog) {
        arm_watchdog_timer(result->watchdog_generation);
    }
    if (result->action == SP_WIFI_SM_SCHEDULE_RECONNECT) {
        arm_reserved_reconnect_timer();
    }
}

static void start_wifi_connect_attempt(void)
{
    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_reconnect_sm_claim_in_flight(&s_sm);
    portEXIT_CRITICAL(&s_wifi_sm_lock);

    sp_backoff_set_in_flight(&s_backoff, true);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconnect failed: %s", esp_err_to_name(err));
        sp_backoff_note_failure(&s_backoff, now_ms());
        sp_backoff_set_in_flight(&s_backoff, false);
        portENTER_CRITICAL(&s_wifi_sm_lock);
        sp_wifi_sm_result_t failed =
            sp_wifi_reconnect_sm_on_connect_start_failed(&s_sm);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        apply_sm_result(&failed);
        return;
    }
    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_sm_result_t accepted =
        sp_wifi_reconnect_sm_on_connect_accepted(&s_sm);
    portEXIT_CRITICAL(&s_wifi_sm_lock);
    apply_sm_result(&accepted);
    ESP_LOGI(
        TAG,
        "Wi-Fi connect started (generation=%lu watchdog=%lums)",
        (unsigned long)accepted.watchdog_generation,
        (unsigned long)s_sm.in_flight_timeout_ms
    );
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    const bool has_ip = sp_wifi_has_ip();
    const bool ota_pending = sp_lifecycle_ota_reboot_pending();
    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_sm_result_t result = sp_wifi_reconnect_sm_on_reconnect_timer(
        &s_sm, has_ip, ota_pending
    );
    portEXIT_CRITICAL(&s_wifi_sm_lock);
    /* Timer already fired; drop handle (SM cleared reconnect_armed). */
    if (s_reconnect_timer != NULL) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    if (result.action == SP_WIFI_SM_START_CONNECT) {
        start_wifi_connect_attempt();
    }
}

static void wifi_in_flight_watchdog_cb(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    if (s_watchdog_timer != NULL) {
        esp_timer_delete(s_watchdog_timer);
        s_watchdog_timer = NULL;
    }
    if (generation != s_watchdog_timer_generation) {
        return;
    }
    s_watchdog_timer_generation = 0;

    const bool ota_pending = sp_lifecycle_ota_reboot_pending();
    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_sm_result_t result =
        sp_wifi_reconnect_sm_on_watchdog(&s_sm, generation, ota_pending);
    portEXIT_CRITICAL(&s_wifi_sm_lock);
    if (result.action == SP_WIFI_SM_NONE && !result.cancel_reconnect_timer
        && !result.cancel_watchdog && !result.arm_watchdog) {
        return;
    }
    if (result.action == SP_WIFI_SM_SCHEDULE_RECONNECT) {
        ESP_LOGW(
            TAG,
            "Wi-Fi in-flight timeout (%lu ms) generation=%lu; clearing in_flight "
            "and scheduling reconnect",
            (unsigned long)s_sm.in_flight_timeout_ms,
            (unsigned long)generation
        );
        sp_backoff_set_in_flight(&s_backoff, false);
        sp_backoff_note_failure(&s_backoff, now_ms());
    }
    apply_sm_result(&result);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *event = data;

    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_sm_result_t result = sp_wifi_reconnect_sm_on_got_ip(&s_sm);
    portEXIT_CRITICAL(&s_wifi_sm_lock);

    if (!result.ip_acquired) {
        ESP_LOGW(
            TAG,
            "ignoring stale GOT_IP " IPSTR " (newer Wi-Fi attempt in progress)",
            IP2STR(&event->ip_info.ip)
        );
        return;
    }

    ESP_LOGI(TAG, "got IPv4 address " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi, GOT_IP);
    apply_sm_result(&result);
    sp_backoff_note_success(&s_backoff, now_ms());
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = data;
        ESP_LOGI(
            TAG,
            "associated with AP ssid='%s' channel=%u "
            "bssid=%02x:%02x:%02x:%02x:%02x:%02x (awaiting DHCP/IP)",
            (const char *)event->ssid,
            (unsigned)event->channel,
            event->bssid[0],
            event->bssid[1],
            event->bssid[2],
            event->bssid[3],
            event->bssid[4],
            event->bssid[5]
        );
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        /* reason 201 = WIFI_REASON_NO_AP_FOUND: keep retrying with backoff, not fatal.
         * reason 2 (AUTH_EXPIRE) and 205 (BEACON_TIMEOUT) use the same generic path. */
        xEventGroupClearBits(s_wifi, GOT_IP);
        sp_backoff_set_in_flight(&s_backoff, false);

        const bool ota_pending = sp_lifecycle_ota_reboot_pending();
        if (ota_pending) {
            ESP_LOGI(TAG, "Wi-Fi disconnect during OTA reboot pending (expected)");
            portENTER_CRITICAL(&s_wifi_sm_lock);
            const bool note_diag =
                sp_wifi_reconnect_sm_take_disconnect_diag(&s_sm);
            sp_wifi_sm_result_t result =
                sp_wifi_reconnect_sm_on_disconnected(&s_sm, true);
            portEXIT_CRITICAL(&s_wifi_sm_lock);
            if (note_diag) {
                sp_diag_note_wifi_disconnect();
            }
            apply_sm_result(&result);
            log_sta_disconnected_rate_limited((int)event->reason);
            return;
        }

        /* Advance backoff on every failed join attempt. Counting only the first
         * disconnect left delays stuck near the 1 s initial value while the AP
         * remained missing. */
        sp_backoff_note_failure(&s_backoff, now_ms());
        portENTER_CRITICAL(&s_wifi_sm_lock);
        const bool note_diag = sp_wifi_reconnect_sm_take_disconnect_diag(&s_sm);
        sp_wifi_sm_result_t result =
            sp_wifi_reconnect_sm_on_disconnected(&s_sm, false);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        if (note_diag) {
            sp_diag_note_wifi_disconnect();
        }
        apply_sm_result(&result);
        log_sta_disconnected_rate_limited((int)event->reason);
    }
}

bool sp_wifi_has_ip(void)
{
    if (s_wifi == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi) & GOT_IP) != 0;
}

esp_err_t sp_wifi_start(const sp_network_t *net)
{
    sp_backoff_config_t bo_cfg;
    sp_backoff_config_wifi(&bo_cfg);
    sp_backoff_init(&s_backoff, &bo_cfg);
    sp_wifi_reconnect_sm_init(&s_sm, SP_WIFI_IN_FLIGHT_TIMEOUT_MS);

    s_wifi = xEventGroupCreate();
    if (s_wifi == NULL) {
        ESP_LOGE(TAG, "failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL
    ));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &on_wifi_event, NULL
    ));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, net->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, net->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "connecting to configured SSID '%s'", net->ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_wifi_connect_attempt();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(SP_WIFI_IN_FLIGHT_TIMEOUT_MS)
    );
    if ((bits & GOT_IP) == 0) {
        ESP_LOGE(TAG, "Wi-Fi timeout");
        sp_backoff_set_in_flight(&s_backoff, false);
        sp_backoff_note_failure(&s_backoff, now_ms());
        portENTER_CRITICAL(&s_wifi_sm_lock);
        sp_wifi_sm_result_t result =
            sp_wifi_reconnect_sm_on_boot_wait_timeout(&s_sm);
        portEXIT_CRITICAL(&s_wifi_sm_lock);
        apply_sm_result(&result);
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

esp_err_t sp_wifi_get_rssi(int *rssi_dbm)
{
    if (rssi_dbm == NULL) return ESP_ERR_INVALID_ARG;
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK) *rssi_dbm = ap_info.rssi;
    return err;
}

void sp_wifi_prepare_ota_reboot(void)
{
    portENTER_CRITICAL(&s_wifi_sm_lock);
    sp_wifi_sm_result_t result = sp_wifi_reconnect_sm_prepare_ota_reboot(&s_sm);
    portEXIT_CRITICAL(&s_wifi_sm_lock);
    apply_sm_result(&result);
    sp_backoff_set_in_flight(&s_backoff, false);
}
