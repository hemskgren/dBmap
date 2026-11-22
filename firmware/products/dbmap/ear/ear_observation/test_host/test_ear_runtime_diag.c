#include "../ear_runtime_diag.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static ear_runtime_snapshot_t s_published;
static atomic_bool s_done;

static void enter(void *ctx)
{
    assert(pthread_mutex_lock(ctx) == 0);
}

static void leave(void *ctx)
{
    assert(pthread_mutex_unlock(ctx) == 0);
}

static const ear_runtime_snapshot_lock_t s_lock = {
    .enter = enter,
    .leave = leave,
    .ctx = &s_mutex,
};

static void *publish_generations(void *unused)
{
    (void)unused;
    ear_runtime_snapshot_t next = {0};
    for (uint32_t i = 1; i <= 20000; ++i) {
        next.values[EAR_RUNTIME_obs_start_attempts] = i;
        /* The SDK call has not returned yet. Health can read the last generation. */
        ear_runtime_snapshot_t previous;
        ear_runtime_snapshot_read(&s_published, &previous, &s_lock);
        assert(previous.valid == (i > 1));
        assert(previous.values[EAR_RUNTIME_obs_start_attempts] == i - 1);
        next.values[EAR_RUNTIME_obs_start_publish_accepted] = i;
        ear_runtime_snapshot_publish(&s_published, &next, &s_lock);
    }
    atomic_store(&s_done, true);
    return NULL;
}

static void test_concurrent_snapshot(void)
{
    pthread_t writer;
    assert(pthread_create(&writer, NULL, publish_generations, NULL) == 0);
    do {
        ear_runtime_snapshot_t read;
        ear_runtime_snapshot_read(&s_published, &read, &s_lock);
        assert(read.values[EAR_RUNTIME_obs_start_attempts] ==
               read.values[EAR_RUNTIME_obs_start_publish_accepted]);
        assert(read.sequence == read.values[EAR_RUNTIME_obs_start_attempts]);
    } while (!atomic_load(&s_done));
    assert(pthread_join(writer, NULL) == 0);

    ear_runtime_snapshot_t next = {0};
    s_published.sequence = UINT32_MAX;
    ear_runtime_snapshot_publish(&s_published, &next, &s_lock);
    assert(s_published.valid && s_published.sequence == 0u);
}

static void test_publish_outcomes(void)
{
    ear_p8_outcomes_t outcomes = {0};
    for (ear_p8_kind_t kind = EAR_P8_START; kind < EAR_P8_KIND_COUNT; ++kind) {
        ear_p8_note_outcome(&outcomes, kind, false, 77);
        ear_p8_note_outcome(&outcomes, kind, true, 1);
        ear_p8_note_outcome(&outcomes, kind, true, 0);
        ear_p8_note_outcome(&outcomes, kind, true, -1);
        ear_p8_note_outcome(&outcomes, kind, true, -2);
        assert(outcomes.count[kind][EAR_P8_PRE_MQTT_FAIL] == 1u);
        assert(outcomes.count[kind][EAR_P8_PUBLISH_ACCEPTED] == 1u);
        assert(outcomes.count[kind][EAR_P8_MQTT_PUBLISH_FAILED] == 3u);
    }
    outcomes.count[EAR_P8_END][EAR_P8_PUBLISH_ACCEPTED] = UINT32_MAX;
    ear_p8_note_outcome(&outcomes, EAR_P8_END, true, 7);
    assert(outcomes.saturated);
    assert(outcomes.count[EAR_P8_END][EAR_P8_PUBLISH_ACCEPTED] == UINT32_MAX);
}

static unsigned s_allocations_left;

static void *limited_alloc(size_t size)
{
    if (s_allocations_left == 0u) {
        return NULL;
    }
    --s_allocations_left;
    return malloc(size);
}

static void test_serialization(void)
{
    ear_runtime_snapshot_t snapshot = {.sequence = 1};
    snapshot.values[EAR_RUNTIME_obs_start_attempts] = 1u;
    cJSON *state = cJSON_CreateObject();
    ear_runtime_diag_attach(state, &snapshot);
    assert(cJSON_GetObjectItem(state, "diag") == NULL);
    snapshot.valid = true;
    ear_runtime_diag_attach(state, &snapshot);
    assert(cJSON_GetObjectItem(state, "diag") == NULL);

    snapshot.values[EAR_RUNTIME_ce_create_dropped_full] = 5u;
    ear_runtime_diag_attach(state, &snapshot);
    cJSON *diag = cJSON_GetObjectItem(state, "diag");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(diag, "ear_p8")) == 1);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(diag, "ce_create_dropped_full")) == 5);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(diag, "obs_start_attempts")) == 1);
    assert(cJSON_GetObjectItem(diag, "obs_start_publish_accepted") == NULL);
    cJSON_Delete(state);

    state = cJSON_CreateObject();
    diag = cJSON_AddObjectToObject(state, "diag");
    cJSON_AddNumberToObject(diag, "mqtt_connect_attempts", 1);
    snapshot.values[EAR_RUNTIME_ce_create_dropped_full] = 0u;
    cJSON_Hooks hooks = {.malloc_fn = limited_alloc, .free_fn = free};
    s_allocations_left = 2;
    cJSON_InitHooks(&hooks);
    ear_runtime_diag_attach(state, &snapshot);
    cJSON_InitHooks(NULL);
    assert(cJSON_GetObjectItem(diag, "ear_p8") == NULL);
    assert(cJSON_GetObjectItem(diag, "obs_start_attempts") == NULL);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(diag, "mqtt_connect_attempts")) == 1);

    for (size_t i = 0; i < EAR_RUNTIME_FIELD_COUNT; ++i) {
        snapshot.values[i] = UINT32_MAX;
    }
    snapshot.sequence = UINT32_MAX;
    char *before = cJSON_PrintUnformatted(state);
    ear_runtime_diag_attach(state, &snapshot);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(diag, "obs_start_attempts")) == UINT32_MAX);
    char *after = cJSON_PrintUnformatted(state);
    const size_t extra = strlen(after) - strlen(before);
    printf("All runtime and legacy fields at UINT32_MAX add %zu JSON bytes\n", extra);
    assert(extra <= 2200u);
    cJSON_free(after);
    cJSON_free(before);
    cJSON_Delete(state);
}

int main(void)
{
    test_publish_outcomes();
    test_concurrent_snapshot();
    test_serialization();
    return 0;
}
