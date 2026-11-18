#pragma once

/**
 * Host-testable MQTT delivery-progress tracker.
 *
 * Verified progress is only:
 * - MQTT PUBACK (MQTT_EVENT_PUBLISHED) for a QoS 1 message id that was
 *   either already tracked or parked as an early PUBACK and later matched
 *   by note_qos1 (PUBACK-before-note race), or
 * - an inbound application publish (MQTT_EVENT_DATA).
 *
 * Not progress: enqueue success alone, QoS 0 acceptance, CONNECTED /
 * SUBSCRIBED / ERROR / DISCONNECTED. PINGRESP is not MQTT_EVENT_DATA.
 *
 * Callers that mutate this state from more than one task must serialize
 * access (see hub_mqtt.c mutex). This module itself is lock-free.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_MQTT_PROGRESS_SLOTS 8
#define SP_MQTT_PROGRESS_EARLY_SLOTS 8
#define SP_MQTT_PROGRESS_LIMIT_MS 90000u

typedef struct {
    int msg_id;
    uint16_t seq;
    bool used;
} sp_mqtt_progress_slot_t;

typedef struct {
    sp_mqtt_progress_slot_t slots[SP_MQTT_PROGRESS_SLOTS];
    int early_acks[SP_MQTT_PROGRESS_EARLY_SLOTS];
    uint8_t early_count;
    uint16_t next_seq;
    bool connected;
    /** 0 until the first verified PUBACK or inbound DATA. */
    int64_t last_verified_ms;
    uint32_t verified_count;
    int64_t deadline_ms;
} sp_mqtt_progress_t;

typedef enum {
    SP_MQTT_PROGRESS_NOTE_REJECTED = 0,
    /** Id stored in outstanding slots. Not verified yet. */
    SP_MQTT_PROGRESS_NOTE_TRACKED = 1,
    /** PUBACK arrived before note; verification completed at note time. */
    SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY = 2,
} sp_mqtt_progress_note_result_t;

typedef enum {
    SP_MQTT_PROGRESS_PUB_IGNORED = 0,
    SP_MQTT_PROGRESS_PUB_VERIFIED = 1,
    /** PUBACK before note_qos1; parked until note matches. */
    SP_MQTT_PROGRESS_PUB_PARKED_EARLY = 2,
} sp_mqtt_progress_pub_result_t;

void sp_mqtt_progress_init(sp_mqtt_progress_t *state);

/** Starts the 90 s deadline. Does not count as verified delivery. */
void sp_mqtt_progress_on_connected(sp_mqtt_progress_t *state, int64_t now_ms);

void sp_mqtt_progress_on_disconnected(sp_mqtt_progress_t *state);

/**
 * Record a QoS 1 publish id (> 0). If a matching early PUBACK was parked,
 * verification completes here (enqueue alone never verifies).
 */
sp_mqtt_progress_note_result_t sp_mqtt_progress_note_qos1(
    sp_mqtt_progress_t *state,
    int msg_id,
    int64_t now_ms
);

/** QoS 0 enqueue is not delivery. This is intentionally a no-op. */
void sp_mqtt_progress_note_qos0(sp_mqtt_progress_t *state);

/**
 * PUBACK. Verifies a tracked id, or parks an early id while connected.
 * Parking does not refresh the deadline until note_qos1 matches.
 */
sp_mqtt_progress_pub_result_t sp_mqtt_progress_on_published(
    sp_mqtt_progress_t *state,
    int msg_id,
    int64_t now_ms
);

/** Inbound MQTT_EVENT_DATA. Ignored when the latch is down. */
void sp_mqtt_progress_on_inbound_data(sp_mqtt_progress_t *state, int64_t now_ms);

bool sp_mqtt_progress_expired(const sp_mqtt_progress_t *state, int64_t now_ms);

/** Age of last verified progress while connected; -1 if none / disconnected. */
int64_t sp_mqtt_progress_age_ms(const sp_mqtt_progress_t *state, int64_t now_ms);

/**
 * Monotonic stamp of last verified PUBACK/DATA. Survives disconnect.
 * 0 if never verified since init.
 */
int64_t sp_mqtt_progress_last_verified_ms(const sp_mqtt_progress_t *state);

#ifdef __cplusplus
}
#endif
