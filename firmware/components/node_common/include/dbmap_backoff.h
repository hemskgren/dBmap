#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Shared exponential backoff for independent network operations.
 *
 * Each MQTT / Wi-Fi / HTTPS caller keeps its own dbmap_backoff_t so failures
 * on one path do not inflate delays on another. Delay math is overflow-safe
 * and host-testable (no FreeRTOS / ESP-IDF dependency in this header).
 */
typedef struct {
    uint32_t initial_ms;
    uint32_t max_ms;
    /** 0–50: full delay is in [d*(1-j/100), d*(1+j/100)]. */
    uint8_t jitter_percent;
    /**
     * After a success, wait this long before clearing failure progression.
     * 0 means reset immediately on note_success (typical for HTTPS).
     */
    uint32_t stable_reset_ms;
} dbmap_backoff_config_t;

typedef struct {
    dbmap_backoff_config_t cfg;
    uint32_t current_delay_ms;
    uint32_t consecutive_failures;
    int64_t next_attempt_at_ms;
    int64_t success_since_ms;
    bool in_flight;
    bool ever_succeeded;
} dbmap_backoff_t;

typedef uint32_t (*dbmap_backoff_rand32_fn)(void);

void dbmap_backoff_config_mqtt(dbmap_backoff_config_t *out);
void dbmap_backoff_config_wifi(dbmap_backoff_config_t *out);
void dbmap_backoff_config_https(dbmap_backoff_config_t *out);

void dbmap_backoff_init(dbmap_backoff_t *state, const dbmap_backoff_config_t *cfg);

/** Immediate clear of failure progression (e.g. successful HTTPS request). */
void dbmap_backoff_reset(dbmap_backoff_t *state);

/**
 * Record a failed attempt at now_ms (monotonic milliseconds).
 * Advances current_delay_ms (capped) and sets next_attempt_at_ms.
 */
void dbmap_backoff_note_failure(dbmap_backoff_t *state, int64_t now_ms);

/**
 * Record a successful attempt. If stable_reset_ms is 0, resets immediately.
 * Otherwise records success_since_ms; call dbmap_backoff_poll_stable() later.
 */
void dbmap_backoff_note_success(dbmap_backoff_t *state, int64_t now_ms);

/**
 * If success has been held for stable_reset_ms, reset failure progression.
 * Returns true when a reset occurred.
 */
bool dbmap_backoff_poll_stable(dbmap_backoff_t *state, int64_t now_ms);

/** Base delay after the latest failure (before jitter). */
uint32_t dbmap_backoff_base_delay_ms(const dbmap_backoff_t *state);

/**
 * Delay to wait after the latest failure, with optional jitter.
 * rand32 may be NULL (no jitter). Result is always in [1, max_ms*1.5] bounds.
 */
uint32_t dbmap_backoff_delay_ms(const dbmap_backoff_t *state, dbmap_backoff_rand32_fn rand32);

bool dbmap_backoff_ready(const dbmap_backoff_t *state, int64_t now_ms);

void dbmap_backoff_set_in_flight(dbmap_backoff_t *state, bool in_flight);
bool dbmap_backoff_is_in_flight(const dbmap_backoff_t *state);

/** Compute next delay after `failures` consecutive failures (overflow-safe). */
uint32_t dbmap_backoff_compute_delay_ms(
    uint32_t initial_ms,
    uint32_t max_ms,
    uint32_t failures
);

#ifdef __cplusplus
}
#endif
