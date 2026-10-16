#include "output_command.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_time.h"
#include "dbmap_types.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "output_cmd";
static const gpio_num_t RELAY_1_GPIO = GPIO_NUM_15;
static const gpio_num_t ONBOARD_LED_GPIO = GPIO_NUM_2;
static const int64_t LED_UPDATE_INTERVAL_US = 50000;
static const int64_t OUTPUT_TEST_PATTERN_PERIOD_US = 2000000;
static portMUX_TYPE s_output_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_output_timer;
static int64_t s_relay_deadline_us;
static int64_t s_relay_started_us;
static bool s_output_test_pattern;

static void update_outputs(void *arg)
{
    (void)arg;
    bool relay_on;
    bool led_on;
    int64_t now = esp_timer_get_time();
    int64_t started_us;
    bool output_test_pattern;

    portENTER_CRITICAL(&s_output_lock);
    if (s_relay_deadline_us != 0 && now >= s_relay_deadline_us) {
        s_relay_deadline_us = 0;
        relay_on = false;
        led_on = false;
    } else if (s_relay_deadline_us != 0) {
        relay_on = true;
        started_us = s_relay_started_us;
        output_test_pattern = s_output_test_pattern;
        portEXIT_CRITICAL(&s_output_lock);

        int64_t elapsed_us = now - started_us;
        if (output_test_pattern) {
            int64_t phase_us = elapsed_us % OUTPUT_TEST_PATTERN_PERIOD_US;
            led_on = phase_us < 150000 || (phase_us >= 350000 && phase_us < 500000);
        } else {
            led_on = (elapsed_us % 1000000) < 500000;
        }
        gpio_set_level(RELAY_1_GPIO, 1);
        gpio_set_level(ONBOARD_LED_GPIO, led_on ? 1 : 0);
        return;
    } else {
        portEXIT_CRITICAL(&s_output_lock);
        return;
    }
    portEXIT_CRITICAL(&s_output_lock);

    gpio_set_level(RELAY_1_GPIO, relay_on ? 1 : 0);
    gpio_set_level(ONBOARD_LED_GPIO, led_on ? 1 : 0);
}

esp_err_t output_command_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << RELAY_1_GPIO) | (1ULL << ONBOARD_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&outputs);
    if (err != ESP_OK) return err;
    err = gpio_set_level(RELAY_1_GPIO, 0);
    if (err != ESP_OK) return err;
    err = gpio_set_level(ONBOARD_LED_GPIO, 0);
    if (err != ESP_OK) return err;

    const esp_timer_create_args_t timer_args = {
        .callback = update_outputs,
        .name = "output_control",
    };
    err = esp_timer_create(&timer_args, &s_output_timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(s_output_timer, LED_UPDATE_INTERVAL_US);
}

static bool start_relay_for_duration(int duration_s, bool output_test_pattern)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_output_lock);
    s_relay_deadline_us = now + (int64_t)duration_s * 1000000;
    s_relay_started_us = now;
    s_output_test_pattern = output_test_pattern;
    portEXIT_CRITICAL(&s_output_lock);

    return gpio_set_level(RELAY_1_GPIO, 1) == ESP_OK
        && gpio_set_level(ONBOARD_LED_GPIO, 1) == ESP_OK;
}

static bool stop_relay(void)
{
    portENTER_CRITICAL(&s_output_lock);
    s_relay_deadline_us = 0;
    s_output_test_pattern = false;
    portEXIT_CRITICAL(&s_output_lock);

    return gpio_set_level(RELAY_1_GPIO, 0) == ESP_OK
        && gpio_set_level(ONBOARD_LED_GPIO, 0) == ESP_OK;
}

void output_handle_command(const char *payload, int len, void *ctx)
{
    const dbmap_effective_t *cfg = ctx;
    char buf[512];
    if (payload == NULL || len <= 0) {
        ESP_LOGW(TAG, "received empty MQTT command");
        return;
    }
    char received_at[24];
    if (dbmap_time_format_utc(received_at, sizeof(received_at)) == ESP_OK) {
        ESP_LOGI(TAG, "received_at=%s MQTT command (%d bytes): %.*s", received_at, len, len, payload);
    } else {
        ESP_LOGI(TAG, "received_at=unsynchronized MQTT command (%d bytes): %.*s", len, len, payload);
    }
    if (len >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "command exceeds the %u-byte limit", (unsigned int)sizeof(buf) - 1);
        return;
    }
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = 0;

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGW(TAG, "command payload is not valid JSON");
        return;
    }
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *act = cJSON_IsString(action) ? action->valuestring : "";
    const cJSON *action_type = cJSON_GetObjectItemCaseSensitive(json, "action_type");
    const char *type = cJSON_IsString(action_type) ? action_type->valuestring : "";
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(json, "duration_s");
    const char *result = "unsupported_action";
    bool output_ok = true;
    int duration_s = 0;

    if (strcmp(act, "RELAY_1_ON") == 0) {
        if (!cJSON_IsNumber(duration) || duration->valuedouble < 1
            || duration->valuedouble > 3600
            || duration->valuedouble != (double)duration->valueint) {
            result = "invalid_duration_s";
        } else {
            duration_s = duration->valueint;
            output_ok = start_relay_for_duration(duration_s, strcmp(type, "output-test") == 0);
            result = output_ok ? "relay_1_on" : "gpio_error";
        }
    } else if (strcmp(act, "RELAY_1_OFF") == 0) {
        output_ok = stop_relay();
        result = output_ok ? "relay_1_off" : "gpio_error";
    }

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "node_id", cfg->identity.node_id);
    cJSON_AddStringToObject(ack, "action", act);
    cJSON_AddStringToObject(ack, "result", result);
    if (cJSON_IsString(action_type)) cJSON_AddStringToObject(ack, "action_type", type);
    if (duration_s > 0) cJSON_AddNumberToObject(ack, "duration_s", duration_s);
    char *s = cJSON_PrintUnformatted(ack);
    if (s != NULL) {
        ESP_LOGI(TAG, "command result action=%s result=%s", act, result);
        dbmap_mqtt_publish_json(cfg->topics.ack, s, 1);
        dbmap_publish_status(cfg, s);
    }
    free(s);
    cJSON_Delete(ack);
    cJSON_Delete(json);
}
