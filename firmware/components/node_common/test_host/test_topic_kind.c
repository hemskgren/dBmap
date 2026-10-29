/**
 * Host unit tests for MQTT topic kind / rebuild helpers.
 *
 * Build/run from repo root (Ear kind):
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/components/node_common/include \
 *     -I firmware/components/node_common/test_host \
 *     -DCONFIG_DBMAP_NODE_TYPE_EAR=1 \
 *     -DCONFIG_DBMAP_HARDWARE_REVISION=\"esp32-s3-n16r8\" \
 *     -DCONFIG_DBMAP_WIFI_SSID=\"\" \
 *     -DCONFIG_DBMAP_WIFI_PASSWORD=\"\" \
 *     -DCONFIG_DBMAP_HUB_HTTP_URL=\"https://hub.local:8443\" \
 *     -DCONFIG_DBMAP_BOOTSTRAP_TOKEN=\"\" \
 *     firmware/components/node_common/identity.c \
 *     firmware/components/node_common/test_host/test_topic_kind.c \
 *     -o /tmp/test_dbmap_topic_kind_ear && /tmp/test_dbmap_topic_kind_ear
 *
 * Output kind: omit -DCONFIG_DBMAP_NODE_TYPE_EAR=1 (defaults to esp-output).
 */
#include "dbmap_types.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

int main(void)
{
    dbmap_effective_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.identity.node_id, "EAR-001", sizeof(cfg.identity.node_id) - 1);
    dbmap_rebuild_topics(&cfg);

#if CONFIG_DBMAP_NODE_TYPE_EAR
    EXPECT(strcmp(dbmap_mqtt_topic_kind(), "esp-ear") == 0);
    EXPECT(strcmp(cfg.topics.hello, "esp-ear/local/ear-001/hello") == 0);
    EXPECT(strcmp(cfg.topics.audio, "esp-ear/local/ear-001/audio") == 0);
    EXPECT(strcmp(cfg.topics.observation, "esp-ear/local/ear-001/observation") == 0);
#else
    EXPECT(strcmp(dbmap_mqtt_topic_kind(), "esp-output") == 0);
    EXPECT(strcmp(cfg.topics.hello, "esp-output/local/ear-001/hello") == 0);
    EXPECT(strcmp(cfg.topics.audio, "esp-output/local/ear-001/audio") == 0);
#endif

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
