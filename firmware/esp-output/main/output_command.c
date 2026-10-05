#include "output_command.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_types.h"
#include "esp_log.h"

static const char *TAG = "output_cmd";

void output_handle_command(const char *payload, int len, void *ctx)
{
    const dbmap_effective_t *cfg = ctx;
    char buf[512];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = 0;

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGW(TAG, "invalid command json");
        return;
    }
    const cJSON *action = cJSON_GetObjectItem(json, "action");
    const char *act = cJSON_IsString(action) ? action->valuestring : "";
    ESP_LOGI(TAG, "command action=%s", act);

    /* Relays and local audio are stubbed until hardware pins/files are mapped. */
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "node_id", cfg->identity.node_id);
    cJSON_AddStringToObject(ack, "action", act);
    cJSON_AddStringToObject(ack, "result", "accepted");
    char *s = cJSON_PrintUnformatted(ack);
    dbmap_mqtt_publish_json(cfg->topics.ack, s, 1);
    dbmap_publish_status(cfg, s);
    free(s);
    cJSON_Delete(ack);
    cJSON_Delete(json);
}
