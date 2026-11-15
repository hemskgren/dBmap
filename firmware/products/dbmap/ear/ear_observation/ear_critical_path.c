/**
 * Critical Observation Path lifecycle (detection disabled by default).
 */
#include "ear_critical_path.h"

#include "ae_signature.h"

#include <string.h>

#ifndef EAR_CRITICAL_PERIODIC_MS_DEFAULT
#define EAR_CRITICAL_PERIODIC_MS_DEFAULT 5000u
#endif
#ifndef EAR_CRITICAL_IDLE_TIMEOUT_MS_DEFAULT
#define EAR_CRITICAL_IDLE_TIMEOUT_MS_DEFAULT 15000u
#endif
#ifndef EAR_CRITICAL_MAX_ACTIVE_MS_DEFAULT
#define EAR_CRITICAL_MAX_ACTIVE_MS_DEFAULT 30000u
#endif
#ifndef EAR_CRITICAL_MATCH_DISTANCE_DEFAULT
#define EAR_CRITICAL_MATCH_DISTANCE_DEFAULT 4.0f
#endif
#ifndef EAR_CRITICAL_MATCH_CREST_FLOOR_DEFAULT
#define EAR_CRITICAL_MATCH_CREST_FLOOR_DEFAULT 12.0f
#endif

void ear_critical_config_set_defaults(ear_critical_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->periodic_ms = EAR_CRITICAL_PERIODIC_MS_DEFAULT;
    cfg->idle_timeout_ms = EAR_CRITICAL_IDLE_TIMEOUT_MS_DEFAULT;
    cfg->max_active_ms = EAR_CRITICAL_MAX_ACTIVE_MS_DEFAULT;
    cfg->match_distance_max = EAR_CRITICAL_MATCH_DISTANCE_DEFAULT;
    cfg->match_crest_floor_db = EAR_CRITICAL_MATCH_CREST_FLOOR_DEFAULT;
    cfg->auto_detect_enabled = false;
}

bool ear_critical_init(ear_critical_path_t *cp, const ear_critical_config_t *cfg)
{
    if (cp == NULL || cfg == NULL || cfg->uuid_fn == NULL) {
        return false;
    }
    memset(cp, 0, sizeof(*cp));
    cp->cfg = *cfg;
    if (cp->cfg.periodic_ms == 0u) {
        cp->cfg.periodic_ms = EAR_CRITICAL_PERIODIC_MS_DEFAULT;
    }
    if (cp->cfg.idle_timeout_ms == 0u) {
        cp->cfg.idle_timeout_ms = EAR_CRITICAL_IDLE_TIMEOUT_MS_DEFAULT;
    }
    if (cp->cfg.max_active_ms == 0u) {
        cp->cfg.max_active_ms = EAR_CRITICAL_MAX_ACTIVE_MS_DEFAULT;
    }
    return true;
}

void ear_critical_reset(ear_critical_path_t *cp)
{
    if (cp == NULL) {
        return;
    }
    ear_critical_diag_t diag = cp->diag;
    ear_critical_config_t cfg = cp->cfg;
    memset(cp, 0, sizeof(*cp));
    cp->cfg = cfg;
    cp->diag = diag;
}

bool ear_critical_is_active(const ear_critical_path_t *cp)
{
    return cp != NULL && cp->active;
}

void ear_critical_get_diag(const ear_critical_path_t *cp, ear_critical_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    if (cp == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = cp->diag;
}

bool ear_critical_active_observation_id(const ear_critical_path_t *cp, char out[EAR_CRITICAL_OBS_ID_LEN])
{
    if (cp == NULL || out == NULL || !cp->active) {
        if (out != NULL) {
            out[0] = '\0';
        }
        return false;
    }
    memcpy(out, cp->observation_id, EAR_CRITICAL_OBS_ID_LEN);
    return true;
}

static bool sig_usable(const ae_signature_t *sig)
{
    return sig != NULL && ae_signature_usable_for_tracking(sig) &&
           sig->quality != AE_QUALITY_INVALID;
}

static bool evidence_matches(const ear_critical_path_t *cp, const ae_signature_t *sig)
{
    if (!sig_usable(sig)) {
        return false;
    }
    if (sig->crest_db < cp->cfg.match_crest_floor_db) {
        return false;
    }
    const float d = ae_signature_distance(&cp->frozen, sig);
    return d <= cp->cfg.match_distance_max;
}

static const char *end_reason_to_termination(ear_critical_end_reason_t reason)
{
    switch (reason) {
    case EAR_CRITICAL_END_OTA:
        return "ota_reboot";
    case EAR_CRITICAL_END_IDLE:
    case EAR_CRITICAL_END_MAX_ACTIVE:
    case EAR_CRITICAL_END_EXPLICIT:
    default:
        return "natural";
    }
}

static void bump_end_diag(ear_critical_path_t *cp, ear_critical_end_reason_t reason)
{
    switch (reason) {
    case EAR_CRITICAL_END_IDLE:
        cp->diag.end_idle++;
        break;
    case EAR_CRITICAL_END_MAX_ACTIVE:
        cp->diag.end_max++;
        break;
    case EAR_CRITICAL_END_OTA:
        cp->diag.end_ota++;
        break;
    case EAR_CRITICAL_END_EXPLICIT:
    default:
        cp->diag.end_explicit++;
        break;
    }
}

/**
 * Terminate lifecycle first, then attempt publish.
 * Publication failure never leaves the binding active.
 */
static void end_active(
    ear_critical_path_t *cp,
    int64_t now_mono_us,
    ear_critical_end_reason_t reason,
    const ae_signature_t *evidence_or_null
)
{
    if (cp == NULL || !cp->active) {
        return;
    }

    char oid[EAR_CRITICAL_OBS_ID_LEN];
    memcpy(oid, cp->observation_id, sizeof(oid));
    const int64_t interval_start = cp->last_report_end_mono_us;
    const int64_t start_mono = cp->start_mono_us;
    const double interval_ms =
        now_mono_us > interval_start ? (double)(now_mono_us - interval_start) / 1000.0 : 0.0;
    const double episode_ms =
        now_mono_us > start_mono ? (double)(now_mono_us - start_mono) / 1000.0 : 0.0;
    const char *term = end_reason_to_termination(reason);
    ear_critical_publish_fn publish = cp->cfg.publish;
    void *user = cp->cfg.publish_user;

    /* Release binding before publish. */
    cp->active = false;
    cp->observation_id[0] = '\0';
    memset(&cp->frozen, 0, sizeof(cp->frozen));
    bump_end_diag(cp, reason);

    if (publish != NULL) {
        const bool ok = publish(
            user,
            oid,
            "end",
            term,
            EAR_CRITICAL_ORIGIN,
            interval_start,
            now_mono_us,
            interval_ms,
            episode_ms,
            evidence_or_null
        );
        if (!ok) {
            cp->diag.publish_fail++;
        }
    } else {
        cp->diag.publish_fail++;
    }
}

static bool start_active(ear_critical_path_t *cp, const ae_signature_t *start_sig, int64_t now_mono_us)
{
    if (cp == NULL || !sig_usable(start_sig) || cp->cfg.uuid_fn == NULL) {
        return false;
    }
    if (cp->active) {
        cp->diag.drop_busy++;
        return false;
    }

    char oid[EAR_CRITICAL_OBS_ID_LEN];
    memset(oid, 0, sizeof(oid));
    cp->cfg.uuid_fn(oid);
    if (oid[0] == '\0') {
        return false;
    }

    cp->active = true;
    memcpy(cp->observation_id, oid, sizeof(cp->observation_id));
    cp->frozen = *start_sig;
    cp->start_mono_us = now_mono_us;
    cp->last_match_mono_us = now_mono_us;
    cp->last_report_end_mono_us = now_mono_us;
    cp->last_periodic_mono_us = now_mono_us;
    cp->diag.start++;

    if (cp->cfg.publish != NULL) {
        const bool ok = cp->cfg.publish(
            cp->cfg.publish_user,
            cp->observation_id,
            "start",
            NULL,
            EAR_CRITICAL_ORIGIN,
            now_mono_us,
            now_mono_us,
            0.0,
            0.0,
            start_sig
        );
        if (!ok) {
            cp->diag.publish_fail++;
            /* START publish failure: still keep lifecycle active so END can run.
             * Identity was minted; Hub may miss START — documented limitation. */
        }
    }
    return true;
}

static void maybe_periodic(ear_critical_path_t *cp, int64_t now_mono_us)
{
    if (!cp->active) {
        return;
    }
    const int64_t period_us = (int64_t)cp->cfg.periodic_ms * 1000LL;
    if (now_mono_us - cp->last_periodic_mono_us < period_us) {
        return;
    }
    const int64_t interval_start = cp->last_report_end_mono_us;
    const double interval_ms =
        now_mono_us > interval_start ? (double)(now_mono_us - interval_start) / 1000.0 : 0.0;
    const double episode_ms =
        now_mono_us > cp->start_mono_us ? (double)(now_mono_us - cp->start_mono_us) / 1000.0
                                        : 0.0;

    if (cp->cfg.publish != NULL) {
        const bool ok = cp->cfg.publish(
            cp->cfg.publish_user,
            cp->observation_id,
            "periodic",
            NULL,
            EAR_CRITICAL_ORIGIN,
            interval_start,
            now_mono_us,
            interval_ms,
            episode_ms,
            &cp->frozen
        );
        if (!ok) {
            cp->diag.publish_fail++;
            /* Keep active; PERIODIC is best-effort. */
            return;
        }
    }
    cp->diag.periodic++;
    cp->last_periodic_mono_us = now_mono_us;
    cp->last_report_end_mono_us = now_mono_us;
}

static void apply_evidence(ear_critical_path_t *cp, const ae_signature_t *sig, int64_t now_mono_us)
{
    if (!cp->active || sig == NULL) {
        return;
    }
    if (evidence_matches(cp, sig)) {
        cp->last_match_mono_us = now_mono_us;
        cp->diag.match_reset++;
    } else {
        cp->diag.bg_ignored++;
    }
}

void ear_critical_on_signature(ear_critical_path_t *cp, const ae_signature_t *sig, int64_t now_mono_us)
{
    if (cp == NULL || sig == NULL) {
        return;
    }
    if (cp->active) {
        /* Continuation / idle-reset only — never a second Observation. */
        apply_evidence(cp, sig, now_mono_us);
        return;
    }
    /*
     * Automatic acoustic START is not implemented in this slice.
     * Even if auto_detect_enabled is set, no production threshold exists;
     * signatures never open a Critical Observation. Use force_start for tests.
     */
    (void)cp->cfg.auto_detect_enabled;
    cp->diag.auto_start_blocked++;
}

void ear_critical_poll(ear_critical_path_t *cp, int64_t now_mono_us)
{
    if (cp == NULL || !cp->active) {
        return;
    }

    const int64_t max_us = (int64_t)cp->cfg.max_active_ms * 1000LL;
    if (now_mono_us - cp->start_mono_us >= max_us) {
        end_active(cp, now_mono_us, EAR_CRITICAL_END_MAX_ACTIVE, NULL);
        return;
    }

    const int64_t idle_us = (int64_t)cp->cfg.idle_timeout_ms * 1000LL;
    if (now_mono_us - cp->last_match_mono_us >= idle_us) {
        end_active(cp, now_mono_us, EAR_CRITICAL_END_IDLE, NULL);
        return;
    }

    maybe_periodic(cp, now_mono_us);
}

bool ear_critical_force_start(ear_critical_path_t *cp, const ae_signature_t *start_sig, int64_t now_mono_us)
{
    return start_active(cp, start_sig, now_mono_us);
}

void ear_critical_inject_evidence(
    ear_critical_path_t *cp,
    const ae_signature_t *sig,
    int64_t now_mono_us
)
{
    if (cp == NULL) {
        return;
    }
    apply_evidence(cp, sig, now_mono_us);
}

void ear_critical_close_for_ota(ear_critical_path_t *cp, int64_t now_mono_us)
{
    if (cp == NULL) {
        return;
    }
    if (cp->active) {
        end_active(cp, now_mono_us, EAR_CRITICAL_END_OTA, NULL);
    }
}

void ear_critical_force_end(
    ear_critical_path_t *cp,
    int64_t now_mono_us,
    ear_critical_end_reason_t reason
)
{
    if (cp == NULL) {
        return;
    }
    if (cp->active) {
        end_active(cp, now_mono_us, reason, NULL);
    }
}
