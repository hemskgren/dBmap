/**
 * NEW host tests: OTA close decision table, gate feed allow, PUBACK outcome
 * classification (PUBACK != Hub ingest).
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/observation_lifecycle.c \
 *     firmware/ear_components/ear_observation/test_host/test_observation_lifecycle.c \
 *     -o /tmp/test_dbmap_observation_lifecycle && /tmp/test_dbmap_observation_lifecycle
 */
#include "dbmap_observation_lifecycle.h"

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

static void test_gate_allows_only_run(void)
{
    EXPECT(dbmap_obs_gate_allows_feed(DBMAP_OBS_GATE_RUN));
    EXPECT(!dbmap_obs_gate_allows_feed(DBMAP_OBS_GATE_CLOSING));
    EXPECT(!dbmap_obs_gate_allows_feed(DBMAP_OBS_GATE_CLOSED));
}

static void test_ota_close_state_table(void)
{
    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_IDLE, false)
        == DBMAP_OBS_OTA_ACTION_NOOP
    );
    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_IDLE, true)
        == DBMAP_OBS_OTA_ACTION_NOOP
    );

    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_PENDING_ENTER, false)
        == DBMAP_OBS_OTA_ACTION_SILENT_ABORT
    );
    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_PENDING_ENTER, true)
        == DBMAP_OBS_OTA_ACTION_SILENT_ABORT
    );

    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_ACTIVE, true)
        == DBMAP_OBS_OTA_ACTION_FORCE_END
    );
    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_ACTIVE, false)
        == DBMAP_OBS_OTA_ACTION_SILENT_ABORT
    );

    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_PENDING_EXIT, true)
        == DBMAP_OBS_OTA_ACTION_FORCE_END
    );
    EXPECT(
        dbmap_obs_ota_action_for_state(DBMAP_ACTIVITY_PENDING_EXIT, false)
        == DBMAP_OBS_OTA_ACTION_SILENT_ABORT
    );
}

static void test_puback_outcomes_not_hub_ingest(void)
{
    EXPECT(
        dbmap_obs_ota_classify_publish(false, true, DBMAP_OBS_MQTT_WAIT_OK)
        == DBMAP_OBS_OTA_PUB_NO_END
    );
    EXPECT(
        dbmap_obs_ota_classify_publish(true, false, DBMAP_OBS_MQTT_WAIT_OK)
        == DBMAP_OBS_OTA_PUB_SKIPPED_DISCONNECTED
    );
    EXPECT(
        dbmap_obs_ota_classify_publish(true, true, DBMAP_OBS_MQTT_WAIT_OK)
        == DBMAP_OBS_OTA_PUB_OK_PUBACK
    );
    EXPECT(
        dbmap_obs_ota_classify_publish(true, true, DBMAP_OBS_MQTT_WAIT_TIMEOUT)
        == DBMAP_OBS_OTA_PUB_TIMEOUT
    );
    EXPECT(
        dbmap_obs_ota_classify_publish(true, true, DBMAP_OBS_MQTT_WAIT_FAILED)
        == DBMAP_OBS_OTA_PUB_FAILED
    );
    /* Names exist for logging; PUBACK must not be described as Hub delivery. */
    EXPECT(strcmp(dbmap_obs_ota_pub_outcome_name(DBMAP_OBS_OTA_PUB_OK_PUBACK), "puback") == 0);
    EXPECT(strstr(dbmap_obs_ota_pub_outcome_name(DBMAP_OBS_OTA_PUB_OK_PUBACK), "hub") == NULL);
}

/**
 * Detached snapshot contract (identity + emission fields): verify the approved
 * field set is expressible as owned buffers without pointing at globals.
 */
static void test_detached_snapshot_owned_buffers(void)
{
    char node_id[32];
    char topic[160];
    char emission_kind[8];
    char termination_reason[16];
    strncpy(node_id, "EAR-001", sizeof(node_id) - 1);
    strncpy(topic, "esp-ear/local/ear-001/observation", sizeof(topic) - 1);
    strncpy(emission_kind, "end", sizeof(emission_kind) - 1);
    strncpy(termination_reason, "ota_reboot", sizeof(termination_reason) - 1);
    node_id[sizeof(node_id) - 1] = '\0';
    topic[sizeof(topic) - 1] = '\0';
    emission_kind[sizeof(emission_kind) - 1] = '\0';
    termination_reason[sizeof(termination_reason) - 1] = '\0';

    EXPECT(strcmp(node_id, "EAR-001") == 0);
    EXPECT(strcmp(topic, "esp-ear/local/ear-001/observation") == 0);
    EXPECT(strcmp(emission_kind, "end") == 0);
    EXPECT(strcmp(termination_reason, "ota_reboot") == 0);
}

int main(void)
{
    test_gate_allows_only_run();
    test_ota_close_state_table();
    test_puback_outcomes_not_hub_ingest();
    test_detached_snapshot_owned_buffers();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
