/**
 * Audio Engine v1 façade: history + multi-listener publish.
 *
 * Device PCM intake (sp_mic_take_pcm_job) is intentionally not hard-wired
 * here yet — wrap behind ae_publish / future ae_poll so Platform can later
 * fan-out windows (§0.9 temporary single-taker constraint).
 */
#pragma once

#include "ae_history.h"
#include "ae_time.h"
#include "ae_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AE_MAX_SIGNATURE_LISTENERS
#define AE_MAX_SIGNATURE_LISTENERS 4u
#endif

typedef void (*ae_signature_listener_t)(const ae_signature_t *sig, void *ctx);

typedef struct {
    uint32_t history_capacity_hint; /* reserved; ring uses AE_HISTORY_CAPACITY */
} ae_config_t;

typedef struct {
    bool started;
    uint32_t last_window_seq;
    int64_t last_window_end_mono_us;
    ae_time_cursor_t time_cursor;
    uint32_t publish_rejected;
    ae_time_status_t last_reject_reason;
    ae_history_t history;
    ae_signature_listener_t listeners[AE_MAX_SIGNATURE_LISTENERS];
    void *listener_ctx[AE_MAX_SIGNATURE_LISTENERS];
    uint8_t listener_count;
} ae_engine_t;

void ae_config_set_defaults(ae_config_t *cfg);
bool ae_start(ae_engine_t *eng, const ae_config_t *cfg);

/**
 * Validate acquisition-time contract, append to history, notify listeners.
 * Rejects missing/non-monotonic timestamps and duplicate/regressing seq.
 * Does not rewrite ``sig->quality`` (gap marking happens at build time).
 */
bool ae_publish_signature(ae_engine_t *eng, const ae_signature_t *sig);

bool ae_get_latest_signature(const ae_engine_t *eng, ae_signature_t *out);
uint32_t ae_history_copy(const ae_engine_t *eng, ae_signature_t *out, uint32_t max_n);
uint32_t ae_history_copy_since(
    const ae_engine_t *eng,
    int64_t min_end_mono_us,
    ae_signature_t *out,
    uint32_t max_n
);
uint32_t ae_history_expire_before_mono(ae_engine_t *eng, int64_t min_end_mono_us);

bool ae_add_signature_listener(
    ae_engine_t *eng,
    ae_signature_listener_t fn,
    void *ctx
);

/**
 * Remove one listener matching both fn and ctx.
 * Compacts the listener array; safe if not found (returns false).
 */
bool ae_remove_signature_listener(
    ae_engine_t *eng,
    ae_signature_listener_t fn,
    void *ctx
);

uint32_t ae_last_window_seq(const ae_engine_t *eng);
int64_t ae_last_window_end_mono_us(const ae_engine_t *eng);
uint32_t ae_publish_rejected_count(const ae_engine_t *eng);
ae_time_status_t ae_last_publish_reject_reason(const ae_engine_t *eng);
uint8_t ae_listener_count(const ae_engine_t *eng);

#ifdef __cplusplus
}
#endif
