#include "ear_runtime_diag.h"

#include <stddef.h>

void ear_p8_note_outcome(
    ear_p8_outcomes_t *outcomes, ear_p8_kind_t kind, bool sdk_called, int message_id
)
{
    const ear_p8_outcome_t outcome = !sdk_called ? EAR_P8_PRE_MQTT_FAIL
                                    : message_id > 0 ? EAR_P8_PUBLISH_ACCEPTED
                                                     : EAR_P8_MQTT_PUBLISH_FAILED;
    uint32_t *count = &outcomes->count[kind][outcome];
    if (*count == UINT32_MAX) {
        outcomes->saturated = true;
    } else {
        ++*count;
    }
}

void ear_runtime_snapshot_publish(
    ear_runtime_snapshot_t *published, const ear_runtime_snapshot_t *next,
    const ear_runtime_snapshot_lock_t *lock
)
{
    lock->enter(lock->ctx);
    const uint32_t sequence = published->sequence + 1u;
    *published = *next;
    published->sequence = sequence;
    published->valid = true;
    lock->leave(lock->ctx);
}

void ear_runtime_snapshot_read(
    const ear_runtime_snapshot_t *published, ear_runtime_snapshot_t *out,
    const ear_runtime_snapshot_lock_t *lock
)
{
    lock->enter(lock->ctx);
    *out = *published;
    lock->leave(lock->ctx);
}

static const struct {
    const char *key;
    bool legacy;
} s_fields[] = {
#define EAR_RUNTIME_FIELD(field, legacy) [EAR_RUNTIME_##field] = {#field, legacy},
#include "ear_runtime_diag_fields.def"
#undef EAR_RUNTIME_FIELD
};

static bool stage_number(cJSON *extension, const char *key, uint32_t value)
{
    cJSON *number = cJSON_CreateNumber(value);
    if (number == NULL) {
        return false;
    }
    cJSON_AddItemToObjectCS(extension, key, number);
    return true;
}

void ear_runtime_diag_attach(cJSON *state, const ear_runtime_snapshot_t *snapshot)
{
    if (!snapshot->valid) {
        return;
    }

    cJSON *diag = cJSON_GetObjectItem(state, "diag");
    if (diag != NULL && !cJSON_IsObject(diag)) {
        return;
    }

    for (size_t i = 0; i < EAR_RUNTIME_FIELD_COUNT; ++i) {
        if (!s_fields[i].legacy || snapshot->values[i] == 0u) {
            continue;
        }
        if (diag == NULL) {
            diag = cJSON_AddObjectToObject(state, "diag");
            if (diag == NULL) {
                return;
            }
        }
        cJSON_AddNumberToObject(diag, s_fields[i].key, snapshot->values[i]);
    }
    if (diag == NULL) {
        return;
    }

    /* Stage the extension so a failed allocation cannot publish a partial
     * generation whose absent counters would be interpreted as zero. */
    cJSON *extension = cJSON_CreateObject();
    if (extension == NULL) {
        return;
    }
    if (!stage_number(extension, "ear_p8", 1)
        || (snapshot->sequence != 0u
            && !stage_number(extension, "ear_p8_seq", snapshot->sequence))) {
        cJSON_Delete(extension);
        return;
    }
    for (size_t i = 0; i < EAR_RUNTIME_FIELD_COUNT; ++i) {
        if (s_fields[i].legacy || snapshot->values[i] == 0u) {
            continue;
        }
        if (!stage_number(extension, s_fields[i].key, snapshot->values[i])) {
            cJSON_Delete(extension);
            return;
        }
    }

    /* Move the staged nodes without allocating or replacing the base diag. */
    while (extension->child != NULL) {
        cJSON *item = cJSON_DetachItemViaPointer(extension, extension->child);
        cJSON_AddItemToObjectCS(diag, item->string, item);
    }
    cJSON_Delete(extension);
}
