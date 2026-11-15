/**
 * Critical Observation Path — EAR-owned lifecycle (ADR 0003 slice).
 *
 * Independent of CE and of the normal Platform episode-registry slots.
 * Capacity: one active Critical Observation. Second trigger while active is
 * dropped and counted (no queue / overflow buffer / merge).
 *
 * Automatic acoustic detection is hard-disabled unless both:
 *   - config.auto_detect_enabled is true, and
 *   - the build enables CONFIG_DBMAP_EAR_OBS_CRITICAL_AUTO_DETECT.
 * Production defaults keep auto-detect off. Lifecycle is exercised via
 * ear_critical_force_start() / ear_critical_inject_evidence().
 *
 * Idle-reset match uses experimental distance/crest-floor parameters — not
 * approved production detector thresholds.
 *
 * END always releases the binding even if report publication fails.
 */
#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAR_CRITICAL_OBS_ID_LEN 37
#define EAR_CRITICAL_ORIGIN "critical_path"

typedef enum {
    EAR_CRITICAL_END_IDLE = 0,
    EAR_CRITICAL_END_MAX_ACTIVE = 1,
    EAR_CRITICAL_END_OTA = 2,
    EAR_CRITICAL_END_EXPLICIT = 3,
} ear_critical_end_reason_t;

typedef struct {
    uint32_t start;
    uint32_t periodic;
    uint32_t end_idle;
    uint32_t end_max;
    uint32_t end_ota;
    uint32_t end_explicit;
    uint32_t drop_busy;
    uint32_t match_reset;
    uint32_t bg_ignored;
    uint32_t publish_fail;
    uint32_t auto_start_blocked;
} ear_critical_diag_t;

/**
 * Publish callback for Critical reports.
 * origin is always EAR_CRITICAL_ORIGIN.
 * termination_reason is NULL for start/periodic; required for end.
 * Return true if publication succeeded (or was intentionally accepted).
 * Lifecycle does not depend on this return for END — binding is already released.
 */
typedef bool (*ear_critical_publish_fn)(
    void *user,
    const char *observation_id,
    const char *emission_kind,
    const char *termination_reason,
    const char *origin,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    double interval_duration_ms,
    double episode_duration_ms,
    const ae_signature_t *evidence_or_null
);

typedef void (*ear_critical_uuid_fn)(char out[EAR_CRITICAL_OBS_ID_LEN]);

typedef struct {
    /** Provisional PERIODIC cadence (implementation default 5000 ms). */
    uint32_t periodic_ms;
    /** Idle timeout; only matching evidence resets (default 15000 ms). */
    uint32_t idle_timeout_ms;
    /** Hard max active duration (default 30000 ms). */
    uint32_t max_active_ms;
    /**
     * Experimental idle-reset match distance (ae_signature_distance).
     * Not an approved production detector parameter.
     */
    float match_distance_max;
    /**
     * Experimental crest floor (dB) required together with distance match.
     * Not an approved production detector parameter.
     */
    float match_crest_floor_db;
    /**
     * When false (production default), acoustic signatures never START a
     * Critical Observation. Inject/force APIs still work.
     */
    bool auto_detect_enabled;
    ear_critical_publish_fn publish;
    void *publish_user;
    ear_critical_uuid_fn uuid_fn;
} ear_critical_config_t;

typedef struct ear_critical_path ear_critical_path_t;

void ear_critical_config_set_defaults(ear_critical_config_t *cfg);

bool ear_critical_init(ear_critical_path_t *cp, const ear_critical_config_t *cfg);

void ear_critical_reset(ear_critical_path_t *cp);

bool ear_critical_is_active(const ear_critical_path_t *cp);

void ear_critical_get_diag(const ear_critical_path_t *cp, ear_critical_diag_t *out);

bool ear_critical_active_observation_id(const ear_critical_path_t *cp, char out[EAR_CRITICAL_OBS_ID_LEN]);

/**
 * Completed-window AE signature input.
 * Auto-START only when auto_detect_enabled. Otherwise may idle-reset if active
 * and evidence matches the frozen fingerprint.
 */
void ear_critical_on_signature(ear_critical_path_t *cp, const ae_signature_t *sig, int64_t now_mono_us);

/**
 * Drive PERIODIC / idle END / max-active END.
 */
void ear_critical_poll(ear_critical_path_t *cp, int64_t now_mono_us);

/**
 * Test / controlled injection: START a Critical Observation with frozen FP.
 * Does not require auto_detect_enabled. Fails (false) if already active
 * (and increments drop_busy) or if uuid/publish hooks missing.
 */
bool ear_critical_force_start(ear_critical_path_t *cp, const ae_signature_t *start_sig, int64_t now_mono_us);

/**
 * Test / controlled injection: feed evidence while active (match or background).
 */
void ear_critical_inject_evidence(
    ear_critical_path_t *cp,
    const ae_signature_t *sig,
    int64_t now_mono_us
);

/**
 * OTA / shutdown: END with ota_reboot if active; always clear binding.
 * Publication failure does not leave the binding active.
 */
void ear_critical_close_for_ota(ear_critical_path_t *cp, int64_t now_mono_us);

/**
 * Explicit END (tests / diagnostics). Always releases binding.
 */
void ear_critical_force_end(
    ear_critical_path_t *cp,
    int64_t now_mono_us,
    ear_critical_end_reason_t reason
);

/** Opaque size for stack/static allocation in tests and firmware. */
struct ear_critical_path {
    ear_critical_config_t cfg;
    bool active;
    char observation_id[EAR_CRITICAL_OBS_ID_LEN];
    ae_signature_t frozen;
    int64_t start_mono_us;
    int64_t last_match_mono_us;
    int64_t last_report_end_mono_us;
    int64_t last_periodic_mono_us;
    ear_critical_diag_t diag;
};

#ifdef __cplusplus
}
#endif
