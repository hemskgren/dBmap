/**
 * Host tests for verified MQTT progress (including PUBACK-before-note).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/mqtt_progress.c \
 *     firmware/platform/node_common/test_host/test_mqtt_progress.c \
 *     -o /tmp/test_mqtt_progress && /tmp/test_mqtt_progress
 */
#include "sp_mqtt_progress.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_connected_is_not_delivery(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 1000);
    EXPECT(state.connected == true);
    EXPECT(state.verified_count == 0);
    EXPECT(sp_mqtt_progress_expired(&state, 1000 + SP_MQTT_PROGRESS_LIMIT_MS - 1) == false);
    EXPECT(sp_mqtt_progress_expired(&state, 1000 + SP_MQTT_PROGRESS_LIMIT_MS) == true);
}

static void test_qos0_and_unknown_without_early(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);
    sp_mqtt_progress_note_qos0(&state);
    EXPECT(state.verified_count == 0);
    /* Disconnected PUBACK is ignored. */
    sp_mqtt_progress_on_disconnected(&state);
    EXPECT(sp_mqtt_progress_on_published(&state, 7, 10) == SP_MQTT_PROGRESS_PUB_IGNORED);
}

static void test_puback_before_note_qos1(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);

    /* Race: PUBACK arrives before note_qos1. */
    EXPECT(
        sp_mqtt_progress_on_published(&state, 42, 100)
        == SP_MQTT_PROGRESS_PUB_PARKED_EARLY
    );
    EXPECT(state.verified_count == 0);
    EXPECT(state.deadline_ms == (int64_t)SP_MQTT_PROGRESS_LIMIT_MS);

    EXPECT(
        sp_mqtt_progress_note_qos1(&state, 42, 150)
        == SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY
    );
    EXPECT(state.verified_count == 1);
    EXPECT(state.last_verified_ms == 150);
    EXPECT(state.deadline_ms == 150 + (int64_t)SP_MQTT_PROGRESS_LIMIT_MS);
    EXPECT(sp_mqtt_progress_expired(&state, 150 + SP_MQTT_PROGRESS_LIMIT_MS - 1) == false);
}

static void test_note_then_puback(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);
    EXPECT(sp_mqtt_progress_note_qos1(&state, 11, 0) == SP_MQTT_PROGRESS_NOTE_TRACKED);
    EXPECT(state.verified_count == 0);
    EXPECT(
        sp_mqtt_progress_on_published(&state, 11, 500)
        == SP_MQTT_PROGRESS_PUB_VERIFIED
    );
    EXPECT(state.verified_count == 1);
}

static void test_multiple_qos1_ids(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);
    EXPECT(sp_mqtt_progress_note_qos1(&state, 11, 0) == SP_MQTT_PROGRESS_NOTE_TRACKED);
    EXPECT(sp_mqtt_progress_note_qos1(&state, 12, 0) == SP_MQTT_PROGRESS_NOTE_TRACKED);
    EXPECT(sp_mqtt_progress_note_qos1(&state, 13, 0) == SP_MQTT_PROGRESS_NOTE_TRACKED);

    EXPECT(sp_mqtt_progress_on_published(&state, 12, 1000) == SP_MQTT_PROGRESS_PUB_VERIFIED);
    EXPECT(state.verified_count == 1);
    EXPECT(sp_mqtt_progress_on_published(&state, 12, 2000) == SP_MQTT_PROGRESS_PUB_PARKED_EARLY);
    EXPECT(state.verified_count == 1);
    EXPECT(sp_mqtt_progress_on_published(&state, 11, 2500) == SP_MQTT_PROGRESS_PUB_VERIFIED);
    EXPECT(sp_mqtt_progress_on_published(&state, 13, 2600) == SP_MQTT_PROGRESS_PUB_VERIFIED);
    EXPECT(state.verified_count == 3);
}

static void test_inbound_and_disconnect(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);
    sp_mqtt_progress_on_inbound_data(&state, 4000);
    EXPECT(state.verified_count == 1);
    (void)sp_mqtt_progress_note_qos1(&state, 4, 4001);
    sp_mqtt_progress_on_disconnected(&state);
    EXPECT(sp_mqtt_progress_expired(&state, 1000000) == false);
    EXPECT(sp_mqtt_progress_on_published(&state, 4, 10) == SP_MQTT_PROGRESS_PUB_IGNORED);
}

static void test_enqueue_never_verifies(void)
{
    sp_mqtt_progress_t state;
    sp_mqtt_progress_init(&state);
    sp_mqtt_progress_on_connected(&state, 0);
    EXPECT(sp_mqtt_progress_note_qos1(&state, 9, 10) == SP_MQTT_PROGRESS_NOTE_TRACKED);
    EXPECT(state.verified_count == 0);
    EXPECT(state.deadline_ms == (int64_t)SP_MQTT_PROGRESS_LIMIT_MS);
}

int main(void)
{
    EXPECT(SP_MQTT_PROGRESS_LIMIT_MS == 90000u);
    test_connected_is_not_delivery();
    test_qos0_and_unknown_without_early();
    test_puback_before_note_qos1();
    test_note_then_puback();
    test_multiple_qos1_ids();
    test_inbound_and_disconnect();
    test_enqueue_never_verifies();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
