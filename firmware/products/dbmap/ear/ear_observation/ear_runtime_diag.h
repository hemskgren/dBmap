#ifndef EAR_RUNTIME_DIAG_H
#define EAR_RUNTIME_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef enum {
    EAR_P8_START,
    EAR_P8_PERIODIC,
    EAR_P8_END,
    EAR_P8_KIND_COUNT,
} ear_p8_kind_t;

typedef enum {
    EAR_P8_PUBLISH_ACCEPTED,
    EAR_P8_MQTT_PUBLISH_FAILED,
    EAR_P8_PRE_MQTT_FAIL,
    EAR_P8_OUTCOME_COUNT,
} ear_p8_outcome_t;

typedef struct {
    uint32_t count[EAR_P8_KIND_COUNT][EAR_P8_OUTCOME_COUNT];
    bool saturated;
} ear_p8_outcomes_t;

typedef enum {
#define EAR_RUNTIME_FIELD(field, legacy) EAR_RUNTIME_##field,
#include "ear_runtime_diag_fields.def"
#undef EAR_RUNTIME_FIELD
    EAR_RUNTIME_FIELD_COUNT,
} ear_runtime_field_t;

typedef struct {
    uint32_t values[EAR_RUNTIME_FIELD_COUNT];
    uint32_t sequence;
    bool valid;
} ear_runtime_snapshot_t;

typedef struct {
    void (*enter)(void *ctx);
    void (*leave)(void *ctx);
    void *ctx;
} ear_runtime_snapshot_lock_t;

void ear_p8_note_outcome(
    ear_p8_outcomes_t *outcomes, ear_p8_kind_t kind, bool sdk_called, int message_id
);
void ear_runtime_snapshot_publish(
    ear_runtime_snapshot_t *published, const ear_runtime_snapshot_t *next,
    const ear_runtime_snapshot_lock_t *lock
);
void ear_runtime_snapshot_read(
    const ear_runtime_snapshot_t *published, ear_runtime_snapshot_t *out,
    const ear_runtime_snapshot_lock_t *lock
);
void ear_runtime_diag_attach(cJSON *state, const ear_runtime_snapshot_t *snapshot);

#endif
