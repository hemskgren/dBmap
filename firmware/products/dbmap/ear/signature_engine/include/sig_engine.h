/**
 * Common Acoustic Signature Engine (Architecture A).
 *
 * One AE window → evaluate all enabled definitions → authoritative per-def
 * state + primary distance match + bounded multi-hit snapshot.
 * CE / Promotion Policy / OM consume via copy/query APIs (no peek).
 */
#pragma once

#include "ae_types.h"
#include "as_profile.h"
#include "ear_siren_se_public.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_ENGINE_MAX_DEFS 16u
#define SIG_ENGINE_MAX_HITS 8u
#define SIG_ENGINE_MAX_TEMPORAL 4u

/**
 * Active EAR classifier package identity (manual bump).
 * Bump when matcher logic, GAP/INVALID temporal rules, Core def parameters,
 * or the compiled enabled Core set can change classification outcomes.
 * Do not bump for unrelated firmware (connectivity, mic transport, etc.).
 * Independent of CONFIG_DBMAP_FIRMWARE_VERSION.
 */
#define EAR_CLASSIFIER_VERSION "1.1"

typedef enum {
    SIG_CAT_CORE = 0,
    SIG_CAT_OPTIONAL = 1,
    SIG_CAT_OWNER = 2,
} sig_category_t;

typedef enum {
    SIG_MATCH_DISTANCE = 0,
    SIG_MATCH_GATES = 1,
} sig_match_mode_t;

typedef enum {
    SIG_RMS_NONE = 0,
    SIG_RMS_ABSOLUTE = 1,
    SIG_RMS_RELATIVE_BG = 2,
} sig_rms_mode_t;

typedef enum {
    SIG_EV_NONE = 0,
    SIG_EV_USABLE = 1,
    SIG_EV_GAP = 2,
    SIG_EV_INVALID = 3,
} sig_evidence_kind_t;

typedef struct {
    uint16_t id;
    const char *name;
    sig_category_t category;
    uint8_t enabled;
    sig_match_mode_t match_mode;
    sig_rms_mode_t rms_mode;
    uint8_t needs_streak;
    uint8_t needs_bg_ema;
    /** For SIG_MATCH_DISTANCE: points at as_profile_t (AS-001). */
    const as_profile_t *distance_profile;
} sig_def_t;

typedef struct {
    uint16_t id;
    uint8_t matched;
    float score;
    float confidence;
    sig_evidence_kind_t last_kind;
    uint8_t gate_vote;
    uint16_t consecutive;
    uint8_t allows_promote;
    uint8_t raw_quality; /* ae_quality_t preserved for diagnostics */
} sig_hit_t;

typedef struct {
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t quality;
    uint8_t hit_count;
    uint8_t overflow;
    uint8_t truncated;
    sig_hit_t hits[SIG_ENGINE_MAX_HITS];
} sig_engine_result_t;

typedef struct {
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t quality;
    uint8_t valid;
    uint16_t profile_id;
    float distance;
    as_acoustic_class_t acoustic_class;
} sig_primary_distance_t;

typedef struct {
    uint16_t id;
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t raw_quality;
    sig_evidence_kind_t last_kind;
    uint8_t matched;
    uint8_t gate_vote;
    float score;
    uint16_t consecutive;
    uint8_t allows_promote;
    uint8_t enabled;
} sig_def_query_t;

/**
 * Init Core library (AS-001, AS-002). Fails if temporal slots exhausted.
 * Idempotent if already started with same config.
 */
bool sig_engine_start(void);

void sig_engine_stop(void);

bool sig_engine_is_started(void);

/** AE listener entry: evaluate all enabled defs for this window, then publish. */
void sig_engine_on_signature(const ae_signature_t *sig);

/** Same as on_signature; used as ae_signature_listener_t. */
void sig_engine_ae_listener(const ae_signature_t *sig, void *ctx);

bool sig_engine_copy_result(sig_engine_result_t *out);

bool sig_engine_query(uint16_t id, sig_def_query_t *out);

bool sig_engine_get_primary_distance_match(sig_primary_distance_t *out);

bool sig_engine_allows_promote(uint16_t id);

/**
 * Coherent AS-002 published snapshot — all fields from one publication.
 * Prefer this for any multi-field decision (e.g. promotion eligibility + streak).
 */
typedef struct {
    uint8_t valid;
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t quality; /* ae_quality_t of the published window */
    ear_siren_last_window_t last_window;
    uint32_t consecutive;
    uint8_t allows_promote;
    uint8_t evidence_ok;
    ear_cls_evidence_t evidence;
} sig_as002_snapshot_t;

/**
 * Copy the full published AS-002 snapshot under one lock acquisition.
 * Returns false if engine not started or no AS-002 window has been published.
 */
bool sig_engine_copy_as002_snapshot(sig_as002_snapshot_t *out);

/**
 * Copy published AS-002 last-window fields under one SIG_PUB_LOCK (no full
 * snapshot temporary). Same publication as other published AS-002 state.
 * Returns false if engine not started or no window has been published yet.
 * For multi-field promote decisions prefer sig_engine_copy_as002_snapshot().
 */
bool sig_engine_copy_as002_last_window(ear_siren_last_window_t *out);

/**
 * Copy published AS-002 progressive-classification evidence under one
 * SIG_PUB_LOCK (no full snapshot temporary). Returns false if evidence was
 * not armed (streak below ALARM) or no AS-002 window has been published.
 * For multi-field promote decisions prefer sig_engine_copy_as002_snapshot().
 */
bool sig_engine_copy_as002_evidence(ear_cls_evidence_t *out);

/** Published AS-002 consecutive streak (0 if unavailable). */
uint32_t sig_engine_as002_consecutive(void);

/** Compiled classifier version string (EAR_CLASSIFIER_VERSION). */
const char *sig_engine_classifier_version(void);

/**
 * Copy EAR_CLASSIFIER_VERSION into a Platform config buffer (≤31 chars + NUL).
 * Does not include Platform headers — caller passes cfg->config.classifier_version.
 */
void sig_engine_apply_classifier_version(char *dst, size_t dst_len);

/** Test / diag: number of enabled definitions. */
uint8_t sig_engine_enabled_count(void);

/**
 * Host-test hook: start engine with `n` distance defs (ids 100..) all using
 * AS-001 profile parameters so overflow/primary tests can exceed MAX_HITS.
 * Not for production builds (gated by SIG_ENGINE_ALLOW_HOST_HOOKS).
 */
bool sig_engine_host_start_as001_clones(uint8_t n);

/**
 * Host-test hook: `n` distance defs (ids 100..) with rms_center ladder so a
 * heli_peak-like signature uniquely best-matches the highest id (outside the
 * first SIG_ENGINE_MAX_HITS vector slots when n > MAX_HITS).
 */
bool sig_engine_host_start_as001_distance_ladder(uint8_t n);

#ifdef __cplusplus
}
#endif
