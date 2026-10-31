#include "dbmap_backoff.h"

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef CONFIG_DBMAP_BACKOFF_MQTT_INITIAL_MS
#define DBMAP_BACKOFF_MQTT_INITIAL_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_MQTT_INITIAL_MS)
#else
#define DBMAP_BACKOFF_MQTT_INITIAL_MS 1000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_MQTT_MAX_MS
#define DBMAP_BACKOFF_MQTT_MAX_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_MQTT_MAX_MS)
#else
#define DBMAP_BACKOFF_MQTT_MAX_MS 60000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_MQTT_STABLE_MS
#define DBMAP_BACKOFF_MQTT_STABLE_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_MQTT_STABLE_MS)
#else
#define DBMAP_BACKOFF_MQTT_STABLE_MS 30000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_WIFI_INITIAL_MS
#define DBMAP_BACKOFF_WIFI_INITIAL_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_WIFI_INITIAL_MS)
#else
#define DBMAP_BACKOFF_WIFI_INITIAL_MS 1000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_WIFI_MAX_MS
#define DBMAP_BACKOFF_WIFI_MAX_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_WIFI_MAX_MS)
#else
#define DBMAP_BACKOFF_WIFI_MAX_MS 60000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_HTTPS_INITIAL_MS
#define DBMAP_BACKOFF_HTTPS_INITIAL_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_HTTPS_INITIAL_MS)
#else
#define DBMAP_BACKOFF_HTTPS_INITIAL_MS 1000u
#endif
#ifdef CONFIG_DBMAP_BACKOFF_HTTPS_MAX_MS
#define DBMAP_BACKOFF_HTTPS_MAX_MS ((uint32_t)CONFIG_DBMAP_BACKOFF_HTTPS_MAX_MS)
#else
#define DBMAP_BACKOFF_HTTPS_MAX_MS 60000u
#endif

static uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

uint32_t dbmap_backoff_compute_delay_ms(
    uint32_t initial_ms,
    uint32_t max_ms,
    uint32_t failures
)
{
    if (initial_ms == 0) {
        initial_ms = 1;
    }
    if (max_ms < initial_ms) {
        max_ms = initial_ms;
    }
    if (failures == 0) {
        return initial_ms;
    }

    uint64_t delay = initial_ms;
    /* failures==1 → initial; each extra failure doubles once. */
    for (uint32_t i = 1; i < failures; ++i) {
        if (delay >= max_ms) {
            return max_ms;
        }
        if (delay > (UINT64_MAX / 2)) {
            return max_ms;
        }
        delay *= 2;
        if (delay >= max_ms) {
            return max_ms;
        }
    }
    return (uint32_t)delay;
}

void dbmap_backoff_config_mqtt(dbmap_backoff_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->initial_ms = DBMAP_BACKOFF_MQTT_INITIAL_MS;
    out->max_ms = DBMAP_BACKOFF_MQTT_MAX_MS;
    out->jitter_percent = 20;
    out->stable_reset_ms = DBMAP_BACKOFF_MQTT_STABLE_MS;
}

void dbmap_backoff_config_wifi(dbmap_backoff_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->initial_ms = DBMAP_BACKOFF_WIFI_INITIAL_MS;
    out->max_ms = DBMAP_BACKOFF_WIFI_MAX_MS;
    out->jitter_percent = 20;
    out->stable_reset_ms = 0;
}

void dbmap_backoff_config_https(dbmap_backoff_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->initial_ms = DBMAP_BACKOFF_HTTPS_INITIAL_MS;
    out->max_ms = DBMAP_BACKOFF_HTTPS_MAX_MS;
    out->jitter_percent = 20;
    out->stable_reset_ms = 0;
}

void dbmap_backoff_init(dbmap_backoff_t *state, const dbmap_backoff_config_t *cfg)
{
    if (state == NULL) {
        return;
    }
    dbmap_backoff_config_t local;
    if (cfg == NULL) {
        dbmap_backoff_config_mqtt(&local);
        cfg = &local;
    }
    state->cfg = *cfg;
    if (state->cfg.initial_ms == 0) {
        state->cfg.initial_ms = 1;
    }
    if (state->cfg.max_ms < state->cfg.initial_ms) {
        state->cfg.max_ms = state->cfg.initial_ms;
    }
    if (state->cfg.jitter_percent > 50) {
        state->cfg.jitter_percent = 50;
    }
    state->current_delay_ms = state->cfg.initial_ms;
    state->consecutive_failures = 0;
    state->next_attempt_at_ms = 0;
    state->success_since_ms = 0;
    state->in_flight = false;
    state->ever_succeeded = false;
}

void dbmap_backoff_reset(dbmap_backoff_t *state)
{
    if (state == NULL) {
        return;
    }
    state->current_delay_ms = state->cfg.initial_ms;
    state->consecutive_failures = 0;
    state->next_attempt_at_ms = 0;
    state->success_since_ms = 0;
    state->in_flight = false;
}

void dbmap_backoff_note_failure(dbmap_backoff_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->in_flight = false;
    state->success_since_ms = 0;
    if (state->consecutive_failures < UINT32_MAX) {
        state->consecutive_failures++;
    }
    state->current_delay_ms = dbmap_backoff_compute_delay_ms(
        state->cfg.initial_ms,
        state->cfg.max_ms,
        state->consecutive_failures
    );
    state->next_attempt_at_ms = now_ms + (int64_t)state->current_delay_ms;
}

void dbmap_backoff_note_success(dbmap_backoff_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->in_flight = false;
    state->ever_succeeded = true;
    state->next_attempt_at_ms = 0;
    if (state->cfg.stable_reset_ms == 0) {
        state->current_delay_ms = state->cfg.initial_ms;
        state->consecutive_failures = 0;
        state->success_since_ms = 0;
        return;
    }
    if (state->success_since_ms == 0) {
        state->success_since_ms = now_ms;
    }
}

bool dbmap_backoff_poll_stable(dbmap_backoff_t *state, int64_t now_ms)
{
    if (state == NULL || state->cfg.stable_reset_ms == 0 || state->success_since_ms == 0) {
        return false;
    }
    if (now_ms - state->success_since_ms < (int64_t)state->cfg.stable_reset_ms) {
        return false;
    }
    state->current_delay_ms = state->cfg.initial_ms;
    state->consecutive_failures = 0;
    state->success_since_ms = 0;
    return true;
}

uint32_t dbmap_backoff_base_delay_ms(const dbmap_backoff_t *state)
{
    if (state == NULL) {
        return 1;
    }
    return state->current_delay_ms ? state->current_delay_ms : state->cfg.initial_ms;
}

uint32_t dbmap_backoff_delay_ms(const dbmap_backoff_t *state, dbmap_backoff_rand32_fn rand32)
{
    if (state == NULL) {
        return 1;
    }
    uint32_t base = dbmap_backoff_base_delay_ms(state);
    uint8_t jitter = state->cfg.jitter_percent;
    if (jitter == 0 || rand32 == NULL) {
        return clamp_u32(base, 1, state->cfg.max_ms);
    }

    uint32_t span = (base * jitter) / 100u;
    if (span == 0) {
        return clamp_u32(base, 1, state->cfg.max_ms);
    }
    uint32_t r = rand32() % (span * 2u + 1u);
    int64_t with_jitter = (int64_t)base - (int64_t)span + (int64_t)r;
    if (with_jitter < 1) {
        with_jitter = 1;
    }
    uint32_t ceiling = state->cfg.max_ms + span;
    if (with_jitter > (int64_t)ceiling) {
        with_jitter = ceiling;
    }
    return (uint32_t)with_jitter;
}

bool dbmap_backoff_ready(const dbmap_backoff_t *state, int64_t now_ms)
{
    if (state == NULL || state->in_flight) {
        return false;
    }
    return now_ms >= state->next_attempt_at_ms;
}

void dbmap_backoff_set_in_flight(dbmap_backoff_t *state, bool in_flight)
{
    if (state != NULL) {
        state->in_flight = in_flight;
    }
}

bool dbmap_backoff_is_in_flight(const dbmap_backoff_t *state)
{
    return state != NULL && state->in_flight;
}
