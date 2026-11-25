/** Compare production host DSP and decision code on fixed and batched PCM spans. */
#include "ae_engine.h"
#include "ae_signature.h"
#include "as_profile.h"
#include "dbmap_activity_detector.h"
#include "ear_sig_product.h"
#include "sig_engine.h"
#include "sp_audio_analyze.h"
#include "sp_audio_metrics.h"
#include "sp_mic_frame_cursor.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    RATE = 16000, FRAMES = 16000, CHUNK = 256, P5_FRAMES = 16384,
    P5_CAP = 16176, WINDOWS = 76
};

static int32_t source[WINDOWS * P5_FRAMES];
static uint32_t uuid_count;

typedef struct {
    unsigned starts, periodics, ends;
    uint32_t start_seq;
    uint64_t candidate;
    int64_t capture_start_us;
    int64_t last_end_us;
    int64_t periodic_start_us, periodic_end_us;
    float periodic_level, end_level;
    bool periodic_has_level, end_has_level;
    ear_obs_end_reason_t reason;
    char last_id[SP_OBS_EPISODE_ID_LEN];
    uint16_t profile;
    ear_cls_assessment_wire_t assessment;
} reports_t;

typedef struct {
    ae_signature_t sig;
    float dynamics_cv;
    float profile_bg, distance;
    bool match;
    uint32_t streak;
    bool tonal, allow;
    uint64_t candidate;
    uint32_t candidate_seq;
    uint32_t associated, created;
    uint8_t rejected_eligibility;
    uint32_t promoted;
    reports_t reports;
} replay_row_t;

typedef enum {
    DIRECT_EXACT,
    CURSOR_EXACT,
    P5_BATCHED,
} assembly_t;

static void uuid(char out[SP_OBS_EPISODE_ID_LEN])
{
    snprintf(out, SP_OBS_EPISODE_ID_LEN, "00000000-0000-4000-8000-%012u", ++uuid_count);
}

static bool start_report(
    void *ctx, const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap, int64_t now
)
{
    (void)now;
    reports_t *r = ctx;
    r->starts++;
    r->candidate = binding->candidate_id;
    r->start_seq = snap->last_window_seq;
    r->profile = snap->relevance_profile_id;
    r->capture_start_us = binding->start_mono_us;
    r->assessment = binding->assessment;
    memcpy(r->last_id, binding->observation_id, sizeof(r->last_id));
    return true;
}

static bool periodic_report(
    void *ctx, const ear_obs_binding_view_t *binding,
    int64_t start, int64_t end, bool has_level, float level
)
{
    reports_t *r = ctx;
    r->periodic_start_us = start;
    r->periodic_end_us = end;
    r->periodic_has_level = has_level;
    r->periodic_level = level;
    assert(strcmp(r->last_id, binding->observation_id) == 0);
    r->periodics++;
    return true;
}

static bool end_report(
    void *ctx, const ear_obs_binding_view_t *binding, ear_obs_end_reason_t reason,
    int64_t start, int64_t end, bool has_level, float level
)
{
    reports_t *r = ctx;
    (void)start;
    r->end_has_level = has_level;
    r->end_level = level;
    assert(strcmp(r->last_id, binding->observation_id) == 0);
    r->ends++;
    r->last_end_us = end;
    r->reason = reason;
    return true;
}

static void make_source(void)
{
    uint32_t rand = 117u;
    /* Quiet broadband, then a bounded noisy tonal packet, then quiet. */
    for (size_t i = 0; i < sizeof(source) / sizeof(source[0]); ++i) {
        rand = rand * 1664525u + 1013904223u;
        const int noise = (int)(rand >> 16) - 32768;
        const bool tone = i >= 4 * FRAMES + 1000u && i < 10 * FRAMES + 1000u;
        const double phase = 6.28318530717958647692 * 344.0 * (double)i / RATE;
        source[i] = tone ? (int32_t)(260000.0 * sin(phase) + 0.15 * noise) : noise / 2;
    }
}

/* P5 representative read-count schedule, not a model of scheduler/DMA time. */
static size_t get_span(
    assembly_t mode, uint32_t seq, sp_mic_frame_cursor_t *cursor,
    size_t *next, int32_t *scratch, int32_t *pcm
)
{
    if (mode != CURSOR_EXACT) {
        const size_t stride = mode == P5_BATCHED ? P5_FRAMES : FRAMES;
        const size_t n = mode == P5_BATCHED ? P5_CAP : FRAMES;
        memcpy(pcm, source + (seq - 1u) * stride, n * sizeof(*pcm));
        return n;
    }
    size_t used = 0;
    while (used < FRAMES) {
        if (cursor->frames == 0) {
            memcpy(scratch, source + *next, CHUNK * sizeof(*scratch));
            *next += CHUNK;
            const sp_mic_frame_read_t read = sp_mic_frame_read(
                cursor, CHUNK * sizeof(*scratch), sizeof(*scratch), true
            );
            assert(!read.incomplete_frame);
        }
        const uint16_t n = (uint16_t)sp_mic_frame_group(cursor, 1, FRAMES - used);
        memcpy(pcm + used, (const uint8_t *)scratch + cursor->offset, n * sizeof(*pcm));
        sp_mic_frame_consume(cursor, n, sizeof(*scratch));
        used += n;
    }
    assert(memcmp(pcm, source + (seq - 1u) * FRAMES, FRAMES * sizeof(*pcm)) == 0);
    return used;
}

static void replay(assembly_t mode, replay_row_t rows[WINDOWS])
{
    sp_audio_analyze_ctx_t dsp;
    sp_audio_analyze_ctx_init(&dsp, RATE);
    as_profile_bg_state_t profile_bg;
    as_profile_bg_init(&profile_bg);
    ae_engine_t ae;
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    assert(ae_start(&ae, &acfg));
    ce_engine_t ce;
    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    assert(ce_start(&ce, &ccfg));
    assert(sig_engine_start());

    reports_t reports = {0};
    uuid_count = 0;
    sp_episode_registry_t registry;
    sp_episode_registry_init(&registry);
    ear_obs_manager_t om;
    ear_promotion_policy_t policy;
    ear_sig_product_t product = {.om = &om, .policy = &policy, .ce = &ce, .ready = true};
    assert(ae_add_signature_listener(&ae, ear_sig_product_on_ae_signature, &product));
    assert(ce_bind_audio_engine(&ce, &ae));
    const ear_obs_manager_config_t mcfg = {
        .registry = &registry, .ae = &ae, .ce = &ce, .uuid_fn = uuid,
        .observation_interval_ms = 5000,
        .emit_start = start_report, .emit_periodic = periodic_report,
        .emit_end = end_report, .emit_ctx = &reports,
    };
    ear_obs_manager_init(&om, &mcfg);
    ce_set_promotion_cb(&ce, ear_obs_manager_on_promotion, &om);
    ear_promotion_policy_init(&policy, &ce, &om);
    ear_promotion_policy_use_sig_engine_as002(&policy, true);

    dbmap_activity_detector_t activity;
    const dbmap_activity_config_t ac = {
        .enter_threshold_dbfs = -45, .exit_threshold_dbfs = -55,
        .min_activity_ms = 1000, .exit_hold_ms = 1000, .observation_interval_ms = 5000,
        .max_invalid_hold_ms = 30000,
    };
    dbmap_activity_detector_init(&activity, &ac);

    int32_t pcm[P5_CAP];
    int32_t scratch[CHUNK];
    sp_mic_frame_cursor_t cursor = {0};
    size_t next = 0u;
    for (uint32_t seq = 1; seq <= WINDOWS; ++seq) {
        replay_row_t *r = &rows[seq - 1u];
        const size_t n = get_span(mode, seq, &cursor, &next, scratch, pcm);
        /* Pin software times to isolate PCM span effects from clock/cadence effects. */
        const int64_t t = (int64_t)seq * 1000000;
        const sp_audio_analyze_input_t input = {
            .pcm_channel_mask = 1, .samples_ch = {pcm}, .sample_count_ch = {n},
            .full_scale_bits = 24, .window_ms = 1000, .window_seq = seq,
            .window_end_mono_us = t,
        };
        sp_audio_window_result_t res;
        assert(sp_audio_analyze_channels(&dsp, &input, 1, &res) == SP_AUDIO_ANALYZE_OK);
        const sp_audio_channel_result_t *ch = &res.channels[0];
        assert(ae_signature_build(seq, &ch->l0, &ch->l1, &ch->l2, NULL, false, &r->sig));
        r->dynamics_cv = ch->l2.envelope_cv;

        as_profile_match_result_t match;
        r->profile_bg = profile_bg.bg_rms_ema;
        r->match = as_profile_match_with_bg(as_profile_as001(), &r->sig, &profile_bg, &match);
        r->distance = match.distance;
        r->rejected_eligibility = match.rejected_eligibility;
        assert(ae_publish_signature(&ae, &r->sig));
        sig_as002_snapshot_t tonal;
        assert(sig_engine_copy_as002_snapshot(&tonal));
        r->streak = tonal.consecutive;
        r->tonal = tonal.last_window.tonal_vote;
        r->allow = tonal.allows_promote;

        /* P5 meters included the suffix that never reached retained DSP. */
        const int32_t *meter_pcm = mode == P5_BATCHED
            ? source + (seq - 1u) * P5_FRAMES : pcm;
        const size_t meter_count = mode == P5_BATCHED ? P5_FRAMES : n;
        double sum_sq = 0.0;
        for (size_t i = 0; i < meter_count; ++i) {
            sum_sq += (double)meter_pcm[i] * meter_pcm[i];
        }
        const int32_t rms_proxy = (int32_t)(sqrt(sum_sq / (double)meter_count) + 0.5);
        const dbmap_activity_sample_t sample = {
            .window_seq = seq, .window_ms = 1000, .measured_at_us = t,
            .ms_since_i2s_enable = seq * 1000, .has_measurement = true,
            .rms_dbfs = sp_audio_rms_dbfs(&rms_proxy, 1, 24), .transport_status = "ok",
            .microphone_status = "signal_present",
        };
        const dbmap_activity_emission_t emit = dbmap_activity_detector_feed(&activity, &sample);
        if (emit.kind == DBMAP_ACTIVITY_EMIT_PERIODIC) {
            ear_obs_manager_on_activity_periodic_hint(
                &om, emit.interval_start_mono_us, emit.interval_end_mono_us, true, emit.signal_level_dbfs
            );
        } else if (emit.kind == DBMAP_ACTIVITY_EMIT_END) {
            ear_obs_manager_on_activity_end_hint(
                &om, EAR_OBS_ACT_END_AMBIENT, emit.interval_start_mono_us,
                emit.interval_end_mono_us, true, emit.signal_level_dbfs
            );
        }
        ear_sig_product_poll(&product, t);
        ear_promotion_policy_diag_t policy_diag;
        ear_promotion_policy_get_diag(&policy, &policy_diag);
        ce_diag_t ce_diag;
        ce_get_diag(&ce, &ce_diag);
        r->associated = ce_diag.associated;
        r->created = ce_diag.created;
        r->candidate = policy_diag.last_candidate_id;
        for (unsigned i = 0; i < CE_MAX_CANDIDATES; ++i) {
            if (ce.slots[i].in_use && ce.slots[i].candidate_id == r->candidate) {
                r->candidate_seq = ce.slots[i].last_window_seq;
            }
        }
        r->promoted = policy_diag.promote_ok;
        r->reports = reports;
    }
    sig_engine_stop();
    ce_stop(&ce);
}

int main(void)
{
    replay_row_t reference[WINDOWS] = {0}, exact[WINDOWS] = {0}, p5[WINDOWS] = {0};
    make_source();
    replay(DIRECT_EXACT, reference);
    replay(CURSOR_EXACT, exact);
    replay(P5_BATCHED, p5);
    unsigned changed = 0;
    for (size_t i = 0; i < WINDOWS; ++i) {
        const replay_row_t *a = &exact[i], *b = &p5[i];
        assert(memcmp(&reference[i].sig, &a->sig, sizeof(a->sig)) == 0);
        assert(reference[i].dynamics_cv == a->dynamics_cv);
        assert(reference[i].match == a->match && reference[i].distance == a->distance);
        assert(reference[i].profile_bg == a->profile_bg);
        assert(reference[i].rejected_eligibility == a->rejected_eligibility);
        assert(reference[i].streak == a->streak && reference[i].allow == a->allow);
        assert(reference[i].tonal == a->tonal);
        assert(reference[i].candidate == a->candidate && reference[i].candidate_seq == a->candidate_seq);
        assert(reference[i].associated == a->associated && reference[i].created == a->created);
        assert(reference[i].promoted == a->promoted);
        assert(memcmp(&reference[i].reports, &a->reports, sizeof(a->reports)) == 0);
        changed += memcmp(&a->sig, &b->sig, sizeof(a->sig)) != 0;
        if (i >= 16u && i != WINDOWS - 1u
            && a->reports.ends == exact[i - 1u].reports.ends
            && b->reports.ends == p5[i - 1u].reports.ends) {
            continue;
        }
        printf("replay seq=%zu P5→P6 rms=%.2f→%.2f flat=%.3f→%.3f cv=%.3f→%.3f "
               "as001_bg=%.2f→%.2f eligible=%d→%d match=%d→%d dist=%.2f→%.2f as002_streak=%u→%u "
               "candidate=%llu/%u→%llu/%u rematches=%u→%u promotions=%u→%u "
               "obs=%u/%u/%u→%u/%u/%u\n",
               i + 1u, b->sig.rms_dbfs, a->sig.rms_dbfs, b->sig.flatness, a->sig.flatness,
               b->dynamics_cv, a->dynamics_cv, b->profile_bg, a->profile_bg,
               !b->rejected_eligibility, !a->rejected_eligibility, b->match, a->match,
               b->distance, a->distance, b->streak, a->streak,
               (unsigned long long)b->candidate, b->candidate_seq,
               (unsigned long long)a->candidate, a->candidate_seq,
               b->associated, a->associated, b->promoted, a->promoted,
               b->reports.starts, b->reports.periodics, b->reports.ends,
               a->reports.starts, a->reports.periodics, a->reports.ends);
    }
    assert(changed != 0u);
    assert(exact[WINDOWS - 1u].reports.starts == 1u);
    assert(exact[WINDOWS - 1u].reports.periodics != 0u);
    assert(exact[WINDOWS - 1u].reports.ends == 1u);
    const reports_t *old = &p5[WINDOWS - 1u].reports;
    const reports_t *new = &exact[WINDOWS - 1u].reports;
    printf("replay publish fields P5→P6 profile=%u→%u start_seq=%u→%u "
           "start_us=%lld→%lld end_us=%lld→%lld end_reason=%d→%d\n",
           old->profile, new->profile, old->start_seq, new->start_seq,
           (long long)old->capture_start_us, (long long)new->capture_start_us,
           (long long)old->last_end_us, (long long)new->last_end_us, old->reason, new->reason);
    printf("replay PERIODIC level P5→P6 %.2f→%.2f, END level %.2f→%.2f\n",
           old->periodic_level, new->periodic_level, old->end_level, new->end_level);
    printf("identical-span parity PASS; changed-span signatures=%u/%u\n", changed, WINDOWS);
    return 0;
}
