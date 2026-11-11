#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-testable Ear boot ordering policy (Sensor Platform Step 1):
 * Platform mic/I2S may start after USB config gates and before Wi-Fi;
 * observation only after Desired.
 *
 * Pure predicates — no I/O. app_main owns the one-shot flags and calls the
 * real start APIs when these return true.
 */
typedef struct {
    /** True after the boot path has invoked (or decided to skip) mic start. */
    bool mic_start_attempted;
    bool desired_boot_done;
    /** True after the boot path has invoked observation start. */
    bool obs_start_attempted;
} dbmap_ear_boot_state_t;

/** Mic may start once the one-shot has not fired (no network gate). */
bool dbmap_ear_boot_allow_mic_start(const dbmap_ear_boot_state_t *s);

/**
 * Observation may start only after boot Desired completes, and only once.
 * Independent of mic success (capture failure still allows publisher start).
 */
bool dbmap_ear_boot_allow_obs_start(const dbmap_ear_boot_state_t *s);

#ifdef __cplusplus
}
#endif
