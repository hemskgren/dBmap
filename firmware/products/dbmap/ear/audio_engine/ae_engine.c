#include "ae_engine.h"

#include <string.h>

void ae_config_set_defaults(ae_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->history_capacity_hint = AE_HISTORY_CAPACITY;
}

bool ae_start(ae_engine_t *eng, const ae_config_t *cfg)
{
    if (eng == NULL) {
        return false;
    }
    (void)cfg;
    memset(eng, 0, sizeof(*eng));
    ae_history_init(&eng->history);
    ae_time_cursor_init(&eng->time_cursor);
    eng->last_reject_reason = AE_TIME_OK;
    eng->started = true;
    return true;
}

bool ae_add_signature_listener(
    ae_engine_t *eng,
    ae_signature_listener_t fn,
    void *ctx
)
{
    if (eng == NULL || !eng->started || fn == NULL) {
        return false;
    }
    if (eng->listener_count >= AE_MAX_SIGNATURE_LISTENERS) {
        return false;
    }
    eng->listeners[eng->listener_count] = fn;
    eng->listener_ctx[eng->listener_count] = ctx;
    eng->listener_count++;
    return true;
}

bool ae_remove_signature_listener(
    ae_engine_t *eng,
    ae_signature_listener_t fn,
    void *ctx
)
{
    if (eng == NULL || !eng->started || fn == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < eng->listener_count; i++) {
        if (eng->listeners[i] == fn && eng->listener_ctx[i] == ctx) {
            for (uint8_t j = i; j + 1u < eng->listener_count; j++) {
                eng->listeners[j] = eng->listeners[j + 1u];
                eng->listener_ctx[j] = eng->listener_ctx[j + 1u];
            }
            eng->listener_count--;
            eng->listeners[eng->listener_count] = NULL;
            eng->listener_ctx[eng->listener_count] = NULL;
            return true;
        }
    }
    return false;
}

bool ae_publish_signature(ae_engine_t *eng, const ae_signature_t *sig)
{
    if (eng == NULL || !eng->started || sig == NULL) {
        return false;
    }

    const ae_time_status_t st = ae_time_validate_publish(&eng->time_cursor, sig);
    if (st != AE_TIME_OK) {
        eng->publish_rejected++;
        eng->last_reject_reason = st;
        return false;
    }

    ae_history_push(&eng->history, sig);
    ae_time_cursor_accept(&eng->time_cursor, sig);
    eng->last_window_seq = sig->window_seq;
    eng->last_window_end_mono_us = sig->window_end_mono_us;
    eng->last_reject_reason = AE_TIME_OK;

    for (uint8_t i = 0; i < eng->listener_count; i++) {
        if (eng->listeners[i] != NULL) {
            eng->listeners[i](sig, eng->listener_ctx[i]);
        }
    }
    return true;
}

bool ae_get_latest_signature(const ae_engine_t *eng, ae_signature_t *out)
{
    if (eng == NULL || !eng->started) {
        return false;
    }
    return ae_history_latest(&eng->history, out);
}

uint32_t ae_history_copy(const ae_engine_t *eng, ae_signature_t *out, uint32_t max_n)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return ae_history_copy_newest(&eng->history, out, max_n);
}

uint32_t ae_history_copy_since(
    const ae_engine_t *eng,
    int64_t min_end_mono_us,
    ae_signature_t *out,
    uint32_t max_n
)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return ae_history_copy_since_mono(&eng->history, min_end_mono_us, out, max_n);
}

uint32_t ae_history_expire_before_mono(ae_engine_t *eng, int64_t min_end_mono_us)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return ae_history_expire_before(&eng->history, min_end_mono_us);
}

uint32_t ae_last_window_seq(const ae_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return eng->last_window_seq;
}

int64_t ae_last_window_end_mono_us(const ae_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return 0;
    }
    return eng->last_window_end_mono_us;
}

uint32_t ae_publish_rejected_count(const ae_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return eng->publish_rejected;
}

ae_time_status_t ae_last_publish_reject_reason(const ae_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return AE_TIME_BAD_IDENTITY;
    }
    return eng->last_reject_reason;
}

uint8_t ae_listener_count(const ae_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    return eng->listener_count;
}
