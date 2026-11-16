/**
 * Per-mic diagnostic error-counter emit policy (firmware reporting only).
 *
 * Controlled by CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS (Kconfig /
 * show_zero_error_counters). Does not affect counter collection.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True when a diagnostic error-counter value should be serialized.
 * show_zero_error_counters=false → only values > 0.
 * show_zero_error_counters=true  → always (including 0).
 */
static inline bool sp_mic_error_counter_should_emit(uint32_t value)
{
#if defined(CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS) \
    && CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS
    (void)value;
    return true;
#else
    return value > 0u;
#endif
}

/** Effective firmware option (for tests / build-metadata alignment). */
static inline bool sp_mic_show_zero_error_counters(void)
{
#if defined(CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS) \
    && CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS
    return true;
#else
    return false;
#endif
}

#ifdef __cplusplus
}
#endif
