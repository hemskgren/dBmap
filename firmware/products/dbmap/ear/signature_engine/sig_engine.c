/**
 * Common Acoustic Signature Engine — see include/sig_engine.h.
 */

#include "sig_engine.h"

#include <string.h>

#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
#include "esp_log.h"
#include <math.h>
#endif

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE s_pub_mux = portMUX_INITIALIZER_UNLOCKED;
#define SIG_PUB_LOCK() portENTER_CRITICAL(&s_pub_mux)
#define SIG_PUB_UNLOCK() portEXIT_CRITICAL(&s_pub_mux)
#else
#define SIG_PUB_LOCK() ((void)0)
#define SIG_PUB_UNLOCK() ((void)0)
#endif

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
    uint8_t temporal_slot; /* UINT8_MAX = none */
} sig_auth_def_t;

/** AS-002 fields required by OM / promotion — published with the window. */
typedef struct {
    uint8_t valid;
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t quality;
    ear_siren_last_window_t last_window;
    uint32_t consecutive;
    uint8_t allows_promote;
    uint8_t evidence_ok;
    ear_cls_evidence_t evidence;
} sig_as002_snap_t;

typedef struct {
    sig_auth_def_t defs[SIG_ENGINE_MAX_DEFS];
    uint8_t def_count;
    sig_primary_distance_t primary;
    sig_engine_result_t result;
    sig_as002_snap_t as002;
} sig_published_t;

typedef struct {
    bool started;
    const sig_def_t *library[SIG_ENGINE_MAX_DEFS];
    uint8_t lib_count;
    uint8_t temporal_used;
    /** Live working detector — never exposed to production consumers. */
    ear_siren_se_public_t as002_live;
    /** AS-001 relative-background EMA (level eligibility). */
    as_profile_bg_state_t as001_bg;
    /* Working area for next window (not visible until publish). */
    sig_published_t work;
    /* Atomically published view. */
    sig_published_t published;
} sig_engine_t;

static sig_engine_t s_eng;

static sig_evidence_kind_t evidence_from_quality(uint8_t q)
{
    if (q == AE_QUALITY_OK || q == AE_QUALITY_DEGRADED) {
        return SIG_EV_USABLE;
    }
    if (q == AE_QUALITY_GAP) {
        return SIG_EV_GAP;
    }
    if (q == AE_QUALITY_INVALID) {
        return SIG_EV_INVALID;
    }
    /* Defensive: never treat unexpected quality as GAP. */
    return SIG_EV_INVALID;
}

static void publish_work(void)
{
    SIG_PUB_LOCK();
    s_eng.published = s_eng.work;
    SIG_PUB_UNLOCK();
}

bool sig_engine_start(void)
{
    if (s_eng.started) {
        return true;
    }
    memset(&s_eng, 0, sizeof(s_eng));

    /* Core library: AS-001 distance+rel-bg, AS-002 gates+temporal. */
    static const sig_def_t k_as001 = {
        .id = AS_PROFILE_ID_AS001,
        .name = "AS-001",
        .category = SIG_CAT_CORE,
        .enabled = 1u,
        .match_mode = SIG_MATCH_DISTANCE,
        .rms_mode = SIG_RMS_RELATIVE_BG,
        .needs_streak = 0u,
        .needs_bg_ema = 1u,
        .distance_profile = NULL, /* filled below after as_profile_as001() */
    };
    /* distance_profile set via non-const setup — use runtime table. */
    static sig_def_t s_as001;
    static sig_def_t s_as002;
    s_as001 = k_as001;
    s_as001.distance_profile = as_profile_as001();

    s_as002.id = AS_PROFILE_ID_AS002;
    s_as002.name = "AS-002";
    s_as002.category = SIG_CAT_CORE;
    s_as002.enabled = 1u;
    s_as002.match_mode = SIG_MATCH_GATES;
    s_as002.rms_mode = SIG_RMS_RELATIVE_BG;
    s_as002.needs_streak = 1u;
    s_as002.needs_bg_ema = 1u;
    s_as002.distance_profile = NULL;

    s_eng.library[0] = &s_as001;
    s_eng.library[1] = &s_as002;
    s_eng.lib_count = 2u;

    uint8_t temporal = 0u;
    for (uint8_t i = 0u; i < s_eng.lib_count; i++) {
        const sig_def_t *d = s_eng.library[i];
        sig_auth_def_t *a = &s_eng.work.defs[i];
        a->id = d->id;
        a->enabled = d->enabled;
        a->temporal_slot = 0xFFu;
        if (d->enabled && (d->needs_streak || d->needs_bg_ema)) {
            if (temporal >= SIG_ENGINE_MAX_TEMPORAL) {
                memset(&s_eng, 0, sizeof(s_eng));
                return false;
            }
            a->temporal_slot = temporal++;
        }
    }
    s_eng.work.def_count = s_eng.lib_count;
    s_eng.temporal_used = temporal;
    ear_siren_se_public_init(&s_eng.as002_live);
    as_profile_bg_init(&s_eng.as001_bg);
    s_eng.published = s_eng.work;
    s_eng.started = true;
    return true;
}

void sig_engine_stop(void)
{
    memset(&s_eng, 0, sizeof(s_eng));
}

bool sig_engine_is_started(void)
{
    return s_eng.started;
}

uint8_t sig_engine_enabled_count(void)
{
    if (!s_eng.started) {
        return 0u;
    }
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < s_eng.lib_count; i++) {
        if (s_eng.library[i] != NULL && s_eng.library[i]->enabled) {
            n++;
        }
    }
    return n;
}

const char *sig_engine_classifier_version(void)
{
    return EAR_CLASSIFIER_VERSION;
}

void sig_engine_apply_classifier_version(char *dst, size_t dst_len)
{
    if (dst == NULL || dst_len == 0u) {
        return;
    }
    const char *v = EAR_CLASSIFIER_VERSION;
    size_t i = 0u;
    while (i + 1u < dst_len && v[i] != '\0') {
        dst[i] = v[i];
        i++;
    }
    dst[i] = '\0';
}

static void eval_distance_def(
    const sig_def_t *d,
    const ae_signature_t *sig,
    sig_auth_def_t *a,
    sig_evidence_kind_t kind
)
{
    a->matched = 0u;
    a->gate_vote = 0u;
    a->score = 0.0f;
    a->allows_promote = 0u;
    a->consecutive = 0u;
    if (kind != SIG_EV_USABLE || d->distance_profile == NULL) {
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
        if (d->id == AS_PROFILE_ID_AS001) {
            ESP_LOGI("ear_dec", "AS001 seq=%lu gate=not_evaluated q=%u",
                     (unsigned long)sig->window_seq, (unsigned)sig->quality);
        }
#endif
        return;
    }
    as_profile_match_result_t r;
    const as_profile_t *p = d->distance_profile;
    bool matched;
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    const as_profile_bg_state_t bg_before = s_eng.as001_bg;
#endif
    if (p->uses_rms_rel_bg) {
        matched = as_profile_match_with_bg(p, sig, &s_eng.as001_bg, &r);
    } else {
        matched = as_profile_match(p, sig, &r);
    }
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    if (d->id == AS_PROFILE_ID_AS001) {
        const bool scored = r.gate != NULL
                            && (!strcmp(r.gate, "match") || !strcmp(r.gate, "distance"));
        ESP_LOGI("ear_dec", "AS001 seq=%lu gate=%s q=%u rms=%.2f bg=%.2f bg_ok=%d min=%.2f dist=%.2f",
                 (unsigned long)sig->window_seq, r.gate != NULL ? r.gate : "unknown",
                 (unsigned)sig->quality, (double)sig->rms_dbfs,
                 bg_before.bg_valid ? (double)bg_before.bg_rms_ema : NAN,
                 bg_before.bg_valid,
                 bg_before.bg_valid ? (double)(bg_before.bg_rms_ema + p->rms_bg_margin_db) : NAN,
                 scored ? (double)r.distance : NAN);
    }
#endif
    if (matched && r.matched) {
        a->matched = 1u;
        a->score = r.distance;
    } else {
        a->score = r.distance;
    }
}

static void eval_gates_as002(
    const ae_signature_t *sig,
    sig_auth_def_t *a,
    sig_evidence_kind_t kind
)
{
    (void)kind;
    /* Production AS-002 algorithm — live working state only (not published). */
    ear_siren_se_public_on_signature(&s_eng.as002_live, sig);

    ear_siren_last_window_t lw;
    ear_siren_se_public_get_last_window(&s_eng.as002_live, &lw);

    if (lw.kind == EAR_SIREN_LAST_INVALID) {
        a->last_kind = SIG_EV_INVALID;
    } else if (sig->quality == AE_QUALITY_GAP) {
        a->last_kind = SIG_EV_GAP;
    } else if (lw.kind == EAR_SIREN_LAST_USABLE) {
        a->last_kind = SIG_EV_USABLE;
    } else if (lw.kind == EAR_SIREN_LAST_UNUSABLE) {
        /* GAP already handled; defensive other → INVALID-like kind. */
        a->last_kind =
            (sig->quality == AE_QUALITY_GAP) ? SIG_EV_GAP : SIG_EV_INVALID;
    } else {
        a->last_kind = SIG_EV_NONE;
    }

    a->gate_vote = (lw.kind == EAR_SIREN_LAST_USABLE && lw.tonal_vote) ? 1u : 0u;
    a->matched = a->gate_vote;
    a->consecutive = (uint16_t)ear_siren_se_public_consecutive(&s_eng.as002_live);
    a->score = (float)a->consecutive;
    a->allows_promote =
        ear_siren_se_public_allows_promote(&s_eng.as002_live) ? 1u : 0u;

    /* Capture AS-002 consumer fields into work snapshot (still unlocked). */
    s_eng.work.as002.valid = 1u;
    s_eng.work.as002.window_seq = sig->window_seq;
    s_eng.work.as002.window_end_mono_us = sig->window_end_mono_us;
    s_eng.work.as002.quality = sig->quality;
    s_eng.work.as002.last_window = lw;
    s_eng.work.as002.consecutive = ear_siren_se_public_consecutive(&s_eng.as002_live);
    s_eng.work.as002.allows_promote =
        ear_siren_se_public_allows_promote(&s_eng.as002_live) ? 1u : 0u;
    s_eng.work.as002.evidence_ok =
        ear_siren_se_public_fill_evidence(
            &s_eng.as002_live, &s_eng.work.as002.evidence
        )
            ? 1u
            : 0u;
}

void sig_engine_on_signature(const ae_signature_t *sig)
{
    if (!s_eng.started || sig == NULL) {
        return;
    }

    /* Build next window entirely in work (unlocked). */
    memset(&s_eng.work.primary, 0, sizeof(s_eng.work.primary));
    memset(&s_eng.work.result, 0, sizeof(s_eng.work.result));
    memset(&s_eng.work.as002, 0, sizeof(s_eng.work.as002));
    s_eng.work.result.window_seq = sig->window_seq;
    s_eng.work.result.window_end_mono_us = sig->window_end_mono_us;
    s_eng.work.result.quality = sig->quality;
    s_eng.work.def_count = s_eng.lib_count;

    const sig_evidence_kind_t kind = evidence_from_quality(sig->quality);

    float best_dist = 0.0f;
    bool have_best = false;
    uint16_t best_id = AS_PROFILE_ID_NONE;
    as_acoustic_class_t best_class = AS_CLASS_UNSPECIFIED;

    uint8_t hit_n = 0u;
    uint8_t qualifiers = 0u;

    for (uint8_t i = 0u; i < s_eng.lib_count; i++) {
        const sig_def_t *d = s_eng.library[i];
        sig_auth_def_t *a = &s_eng.work.defs[i];
        const uint8_t tslot = a->temporal_slot;
        memset(a, 0, sizeof(*a));
        a->id = d->id;
        a->enabled = d->enabled;
        a->temporal_slot = tslot;
        a->window_seq = sig->window_seq;
        a->window_end_mono_us = sig->window_end_mono_us;
        a->raw_quality = sig->quality;
        a->last_kind = kind;

        if (!d->enabled) {
            continue;
        }

        if (d->match_mode == SIG_MATCH_DISTANCE) {
            eval_distance_def(d, sig, a, kind);
            if (a->matched) {
                if (!have_best || a->score < best_dist
                    || (a->score == best_dist && a->id < best_id)) {
                    have_best = true;
                    best_dist = a->score;
                    best_id = a->id;
                    if (d->distance_profile != NULL) {
                        best_class = d->distance_profile->acoustic_class;
                    }
                }
            }
        } else if (d->match_mode == SIG_MATCH_GATES
                   && d->id == AS_PROFILE_ID_AS002) {
            eval_gates_as002(sig, a, kind);
        }

        const bool is_hit =
            (a->last_kind == SIG_EV_USABLE)
            && (a->matched != 0u || a->gate_vote != 0u);
        if (is_hit) {
            qualifiers++;
            if (hit_n < SIG_ENGINE_MAX_HITS) {
                sig_hit_t *h = &s_eng.work.result.hits[hit_n++];
                h->id = a->id;
                h->matched = a->matched;
                h->score = a->score;
                h->confidence = 0.0f;
                h->last_kind = a->last_kind;
                h->gate_vote = a->gate_vote;
                h->consecutive = a->consecutive;
                h->allows_promote = a->allows_promote;
                h->raw_quality = a->raw_quality;
            }
        }
    }

    s_eng.work.result.hit_count = hit_n;
    if (qualifiers > SIG_ENGINE_MAX_HITS) {
        s_eng.work.result.overflow = 1u;
        s_eng.work.result.truncated = 1u;
    }

    s_eng.work.primary.window_seq = sig->window_seq;
    s_eng.work.primary.window_end_mono_us = sig->window_end_mono_us;
    s_eng.work.primary.quality = sig->quality;
    s_eng.work.primary.valid = have_best ? 1u : 0u;
    s_eng.work.primary.profile_id = best_id;
    s_eng.work.primary.distance = best_dist;
    s_eng.work.primary.acoustic_class = best_class;

    publish_work();
}

void sig_engine_ae_listener(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    sig_engine_on_signature(sig);
}

bool sig_engine_copy_result(sig_engine_result_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    SIG_PUB_LOCK();
    *out = s_eng.published.result;
    SIG_PUB_UNLOCK();
    return true;
}

bool sig_engine_query(uint16_t id, sig_def_query_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    SIG_PUB_LOCK();
    for (uint8_t i = 0u; i < s_eng.published.def_count; i++) {
        const sig_auth_def_t *a = &s_eng.published.defs[i];
        if (a->id == id) {
            out->id = a->id;
            out->window_seq = a->window_seq;
            out->window_end_mono_us = a->window_end_mono_us;
            out->raw_quality = a->raw_quality;
            out->last_kind = a->last_kind;
            out->matched = a->matched;
            out->gate_vote = a->gate_vote;
            out->score = a->score;
            out->consecutive = a->consecutive;
            out->allows_promote = a->allows_promote;
            out->enabled = a->enabled;
            SIG_PUB_UNLOCK();
            return true;
        }
    }
    SIG_PUB_UNLOCK();
    return false;
}

bool sig_engine_get_primary_distance_match(sig_primary_distance_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    SIG_PUB_LOCK();
    *out = s_eng.published.primary;
    SIG_PUB_UNLOCK();
    return out->valid != 0u;
}

bool sig_engine_allows_promote(uint16_t id)
{
    sig_def_query_t q;
    if (!sig_engine_query(id, &q)) {
        return false;
    }
    return q.allows_promote != 0u;
}

bool sig_engine_copy_as002_snapshot(sig_as002_snapshot_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    SIG_PUB_LOCK();
    if (!s_eng.published.as002.valid) {
        SIG_PUB_UNLOCK();
        return false;
    }
    out->valid = s_eng.published.as002.valid;
    out->window_seq = s_eng.published.as002.window_seq;
    out->window_end_mono_us = s_eng.published.as002.window_end_mono_us;
    out->quality = s_eng.published.as002.quality;
    out->last_window = s_eng.published.as002.last_window;
    out->consecutive = s_eng.published.as002.consecutive;
    out->allows_promote = s_eng.published.as002.allows_promote;
    out->evidence_ok = s_eng.published.as002.evidence_ok;
    out->evidence = s_eng.published.as002.evidence;
    SIG_PUB_UNLOCK();
    return true;
}

bool sig_engine_copy_as002_last_window(ear_siren_last_window_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    /* Targeted published fields only — one lock, no full snapshot temp. */
    SIG_PUB_LOCK();
    if (!s_eng.published.as002.valid) {
        SIG_PUB_UNLOCK();
        return false;
    }
    *out = s_eng.published.as002.last_window;
    SIG_PUB_UNLOCK();
    return true;
}

bool sig_engine_copy_as002_evidence(ear_cls_evidence_t *out)
{
    if (out == NULL || !s_eng.started) {
        return false;
    }
    /* Targeted published fields only — one lock, no full snapshot temp. */
    SIG_PUB_LOCK();
    if (!s_eng.published.as002.valid) {
        SIG_PUB_UNLOCK();
        return false;
    }
    if (!s_eng.published.as002.evidence_ok) {
        SIG_PUB_UNLOCK();
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = s_eng.published.as002.evidence;
    SIG_PUB_UNLOCK();
    return true;
}

uint32_t sig_engine_as002_consecutive(void)
{
    sig_as002_snapshot_t snap;
    if (!sig_engine_copy_as002_snapshot(&snap)) {
        return 0u;
    }
    return snap.consecutive;
}

#if defined(SIG_ENGINE_ALLOW_HOST_HOOKS)
bool sig_engine_host_start_as001_clones(uint8_t n)
{
    if (n == 0u || n > SIG_ENGINE_MAX_DEFS) {
        return false;
    }
    sig_engine_stop();
    memset(&s_eng, 0, sizeof(s_eng));

    static sig_def_t clones[SIG_ENGINE_MAX_DEFS];
    static as_profile_t profiles[SIG_ENGINE_MAX_DEFS];
    const as_profile_t *p = as_profile_as001();
    for (uint8_t i = 0u; i < n; i++) {
        /*
         * Host multi-hit harness: absolute RMS distance, no relative-bg
         * (production AS-001 uses rel-bg; these clones only test hit vectors).
         */
        profiles[i] = *p;
        profiles[i].uses_rms_rel_bg = false;
        profiles[i].uses_rms = true;
        profiles[i].rms_min_dbfs = -60.0f;
        profiles[i].rms_center_dbfs = -10.0f;
        clones[i].id = (uint16_t)(100u + i);
        clones[i].name = "AS001-clone";
        clones[i].category = SIG_CAT_CORE;
        clones[i].enabled = 1u;
        clones[i].match_mode = SIG_MATCH_DISTANCE;
        clones[i].rms_mode = SIG_RMS_ABSOLUTE;
        clones[i].needs_streak = 0u;
        clones[i].needs_bg_ema = 0u;
        clones[i].distance_profile = &profiles[i];
        s_eng.library[i] = &clones[i];
        s_eng.work.defs[i].id = clones[i].id;
        s_eng.work.defs[i].enabled = 1u;
        s_eng.work.defs[i].temporal_slot = 0xFFu;
    }
    s_eng.lib_count = n;
    s_eng.work.def_count = n;
    as_profile_bg_init(&s_eng.as001_bg);
    s_eng.published = s_eng.work;
    s_eng.started = true;
    return true;
}
bool sig_engine_host_start_as001_distance_ladder(uint8_t n)
{
    if (n == 0u || n > SIG_ENGINE_MAX_DEFS) {
        return false;
    }
    sig_engine_stop();
    memset(&s_eng, 0, sizeof(s_eng));

    static sig_def_t clones[SIG_ENGINE_MAX_DEFS];
    static as_profile_t profiles[SIG_ENGINE_MAX_DEFS];
    const as_profile_t *base = as_profile_as001();
    for (uint8_t i = 0u; i < n; i++) {
        profiles[i] = *base;
        profiles[i].uses_rms_rel_bg = false;
        profiles[i].uses_rms = true;
        profiles[i].rms_min_dbfs = -60.0f;
        /*
         * Ladder rms_center: highest index closest to heli peak (−10).
         * For sig rms=−10, clone n-1 has uniquely lowest distance.
         */
        profiles[i].rms_center_dbfs =
            -10.0f - (float)(n - 1u - i) * 1.0f;
        clones[i].id = (uint16_t)(100u + i);
        clones[i].name = "AS001-ladder";
        clones[i].category = SIG_CAT_CORE;
        clones[i].enabled = 1u;
        clones[i].match_mode = SIG_MATCH_DISTANCE;
        clones[i].rms_mode = SIG_RMS_ABSOLUTE;
        clones[i].needs_streak = 0u;
        clones[i].needs_bg_ema = 0u;
        clones[i].distance_profile = &profiles[i];
        s_eng.library[i] = &clones[i];
        s_eng.work.defs[i].id = clones[i].id;
        s_eng.work.defs[i].enabled = 1u;
        s_eng.work.defs[i].temporal_slot = 0xFFu;
    }
    s_eng.lib_count = n;
    s_eng.work.def_count = n;
    as_profile_bg_init(&s_eng.as001_bg);
    s_eng.published = s_eng.work;
    s_eng.started = true;
    return true;
}
#else
bool sig_engine_host_start_as001_clones(uint8_t n)
{
    (void)n;
    return false;
}

bool sig_engine_host_start_as001_distance_ladder(uint8_t n)
{
    (void)n;
    return false;
}
#endif
