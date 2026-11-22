#include "dbmap_ear_observation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "dbmap_activity_detector.h"
#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l2.h"
#include "sp_diag.h"
#include "ae_device.h"
#include "ce_engine.h"
#include "ear_obs_lifecycle_section.h"
#include "ear_obs_manager.h"
#include "ear_obs_start_retain.h"
#include "ear_promotion_policy.h"
#include "ear_sig_product.h"
#include "sig_engine.h"
#include "ear_critical_path.h"
#include "ear_obs_ota_cleanup.h"
#include "ae_engine.h"
#include "dbmap_ear_acoustic.h"
#include "sp_mic.h"
#include "sp_mic_window_job.h"
#include "ear_runtime_diag.h"
#include "sp_episode_registry.h"
#include "sp_health.h"
#include "dbmap_lifecycle_v2.h"
#include "sp_mqtt.h"
#include "sp_observation_lifecycle.h"
#include "sp_observation_seq.h"
#include "sp_ota.h"
#include "sp_time.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

/** Bounded PUBACK wait for OTA END only (normal Observations do not wait). */
static const uint32_t kOtaEndAckTimeoutMs = 5000;

#if CONFIG_DBMAP_EAR_OBS_L0
/* Static capacity from Kconfig; must cover configured PERIODIC interval (+2 edge). */
#define EAR_OBS_L0_HIST_CAP ((uint32_t)CONFIG_DBMAP_EAR_OBS_L0_MAX_WINDOWS)
#define EAR_OBS_L0_WINDOWS_NEEDED                                              \
    (((uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS / (uint32_t)CONFIG_SP_MIC_WINDOW_MS) + 2u)
#endif

static const char *TAG = "ear_obs";

#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
/* Defined with L0 ring code; L2 Kconfig depends on L0. */
static void l2_continuity_break(void);
static void l2_finalize_interval(
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool force_overflow,
    dbmap_l2_interval_t *out_iv
);
#endif

static const char *NVS_NS = "ear_obs";
static const char *NVS_KEY_SEQ_LIMIT = "seq_lim";
static const char *NVS_KEY_SEQ_LEGACY = "seq"; /* Phase 1 next-value; migrated as limit */

static const sp_effective_t *s_cfg;
static sp_obs_seq_t s_seq;
static bool s_started;
static bool s_test_published;
static bool s_logged_wait_mqtt;
static bool s_ota_close_registered;
static SemaphoreHandle_t s_lifecycle_mu;
static sp_obs_emit_gate_t s_emit_gate = SP_OBS_GATE_RUN;

/* R0 wall-clock stage timers (esp_timer_get_time); aggregated only. */
static uint32_t s_r0_promo_mgr_n;
static uint32_t s_r0_promo_mgr_us_last;
static uint32_t s_r0_promo_mgr_us_max;
static uint64_t s_r0_promo_mgr_us_sum;
static uint32_t s_r0_publish_n;
static uint32_t s_r0_publish_us_last;
static uint32_t s_r0_publish_us_max;
static uint64_t s_r0_publish_us_sum;

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static bool s_logged_mqtt_drop;
static uint32_t s_dropped_while_mqtt_down;
static dbmap_activity_detector_t s_detector;
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
static dbmap_lifecycle_v2_t s_lifecycle_v2;
static uint32_t s_lc_v2_feed_us_last;
static uint32_t s_lc_v2_feed_us_max;
static uint32_t s_lc_v2_feed_count;
#endif
static sp_episode_registry_t s_registry;
static sp_ep_handle_t s_rms_handle;
static bool s_rms_bound;
static bool s_mqtt_episode_rearm_required;
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
static ce_engine_t s_ce;
static ear_obs_manager_t s_obs_mgr;
static ear_promotion_policy_t s_promo_pol;
static bool s_obs_mgr_ready;
static bool s_sig_engine_listener_bound;
static ear_sig_product_t s_sig_product;
#if defined(CONFIG_DBMAP_EAR_AE_DIAG) && CONFIG_DBMAP_EAR_AE_DIAG
static uint32_t s_obs_mgr_uart_diag_last_sig;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
static ear_critical_path_t s_critical;
static bool s_critical_ready;
static bool s_critical_listener_bound;
#endif
#endif
#define EAR_OBS_PRODUCER_RMS 1u
#define EAR_OBS_PRODUCER_MGR 2u

typedef struct {
    bool should_publish;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
    char node_id[DBMAP_NODE_ID_MAX];
    char hub_id[DBMAP_HUB_ID_MAX];
    char topic[DBMAP_TOPIC_MAX];
    char emission_kind[16];
    char termination_reason[16];
    bool has_signal_level_dbfs;
    double signal_level_dbfs;
    double interval_duration_ms;
    double episode_duration_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
    bool has_measurements;
    dbmap_l0_interval_t measurements;
    bool has_signature;
    dbmap_l1_interval_t signature;
    bool has_dynamics;
    dbmap_l2_interval_t dynamics;
} ear_obs_end_snapshot_t;
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
static int64_t s_diag_last_snap_us;
static int64_t s_diag_last_counters_us;
/* Lifetime-min free stack (bytes) sampled at phase checkpoints — not instant usage. */
static uint32_t s_stack_hwm_bytes_pre_analyze;
static uint32_t s_stack_hwm_bytes_post_analyze;
static uint32_t s_stack_hwm_bytes_pre_promo;
static uint32_t s_stack_hwm_bytes_post_promo;
static uint32_t s_stack_hwm_bytes_pre_om_publish;
static uint32_t s_stack_hwm_bytes_post_om_publish;
static uint32_t s_stack_hwm_bytes_pre_ae_diag;
static uint32_t s_stack_hwm_bytes_post_ae_diag;
static uint32_t s_stack_hwm_bytes_post_publish; /* legacy RMS handle_emission only */
static uint32_t s_stack_hwm_bytes_post_diag;
static bool s_stack_hwm_have_pre_analyze;
static bool s_stack_hwm_have_post_analyze;
static bool s_stack_hwm_have_pre_promo;
static bool s_stack_hwm_have_post_promo;
static bool s_stack_hwm_have_pre_om_publish;
static bool s_stack_hwm_have_post_om_publish;
static bool s_stack_hwm_have_pre_ae_diag;
static bool s_stack_hwm_have_post_ae_diag;
static bool s_stack_hwm_have_post_publish;
static bool s_stack_hwm_have_post_diag;
/** Running lifetime minimum observed by ear_obs checkpoints (bytes). */
static uint32_t s_stack_hwm_bytes_min;
static bool s_stack_hwm_min_init;

/**
 * Sample lifetime-min free stack at a phase boundary.
 * On a new minimum, log once (integer-only) then re-sample so any stack used by
 * that log line is attributed to phase "hwm_attrib_log", not the next checkpoint.
 * Observation: decrease is detected after the phase returns — not mid-call.
 */
static void ear_obs_stack_hwm_checkpoint(
    const char *phase,
    const char *emit_kind,
    uint32_t *slot,
    bool *have
)
{
    /* ESP-IDF Xtensa: uxTaskGetStackHighWaterMark returns bytes. */
    const uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (slot != NULL && have != NULL) {
        *slot = hwm;
        *have = true;
    }

    if (!s_stack_hwm_min_init) {
        s_stack_hwm_bytes_min = hwm;
        s_stack_hwm_min_init = true;
        return;
    }
    if (hwm >= s_stack_hwm_bytes_min) {
        return;
    }

    const uint32_t prev = s_stack_hwm_bytes_min;
    s_stack_hwm_bytes_min = hwm;
    const int64_t now_us = esp_timer_get_time();
    ESP_LOGI(
        TAG,
        "ear_obs stack_hwm_new_min bytes=%lu prev=%lu phase=%s emit=%s "
        "mono_us=%lld (observed after phase; lifetime min)",
        (unsigned long)hwm,
        (unsigned long)prev,
        phase != NULL ? phase : "?",
        emit_kind != NULL ? emit_kind : "none",
        (long long)now_us
    );

    /* Logger may deepen the stack; capture that under an explicit phase name. */
    const uint32_t after_log = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (after_log < s_stack_hwm_bytes_min) {
        const uint32_t prev_log = s_stack_hwm_bytes_min;
        s_stack_hwm_bytes_min = after_log;
        ESP_LOGI(
            TAG,
            "ear_obs stack_hwm_new_min bytes=%lu prev=%lu phase=hwm_attrib_log "
            "emit=%s mono_us=%lld (attrib log self-cost)",
            (unsigned long)after_log,
            (unsigned long)prev_log,
            emit_kind != NULL ? emit_kind : "none",
            (long long)esp_timer_get_time()
        );
    }
}

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
/** OM mgr_emit_* path only — before (after=false) or after (after=true) build/publish. */
static void ear_obs_stack_hwm_om_publish(const char *emit_kind, bool after)
{
    if (after) {
        ear_obs_stack_hwm_checkpoint(
            "post_om_publish",
            emit_kind,
            &s_stack_hwm_bytes_post_om_publish,
            &s_stack_hwm_have_post_om_publish
        );
    } else {
        ear_obs_stack_hwm_checkpoint(
            "pre_om_publish",
            emit_kind,
            &s_stack_hwm_bytes_pre_om_publish,
            &s_stack_hwm_have_pre_om_publish
        );
    }
}
#endif /* AUDIO_ENGINE */
#endif /* SERIAL_DIAG */
#if CONFIG_DBMAP_EAR_OBS_L0
typedef struct {
    dbmap_l0_window_t window;
    uint8_t usable; /* detector usable gate at capture association time */
} ear_obs_l0_ring_entry_t;

static ear_obs_l0_ring_entry_t s_l0_ring[EAR_OBS_L0_HIST_CAP];
static uint32_t s_l0_ring_head;
static uint32_t s_l0_ring_count;
static uint32_t s_l0_last_window_seq;
static float s_l0_hist_rms[EAR_OBS_L0_HIST_CAP];
static float s_l0_hist_hl[EAR_OBS_L0_HIST_CAP];
static uint8_t s_l0_hist_hl_valid[EAR_OBS_L0_HIST_CAP];
static int64_t s_l0_hist_t_end[EAR_OBS_L0_HIST_CAP];
static float s_l0_hist_crest[EAR_OBS_L0_HIST_CAP];
static uint8_t s_l0_hist_crest_ok[EAR_OBS_L0_HIST_CAP];
static dbmap_l0_interval_t s_l0_last_interval;
static bool s_l0_last_interval_valid;
static bool s_l0_capacity_ok;
static uint32_t s_l0_hist_overflow;
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
typedef struct {
    dbmap_l1_window_t window;
    uint8_t usable;
} ear_obs_l1_ring_entry_t;

static ear_obs_l1_ring_entry_t s_l1_ring[EAR_OBS_L0_HIST_CAP];
static uint32_t s_l1_ring_head;
static uint32_t s_l1_ring_count;
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
typedef struct {
    dbmap_l2_window_t window;
    uint8_t usable;
} ear_obs_l2_ring_entry_t;

static ear_obs_l2_ring_entry_t s_l2_ring[EAR_OBS_L0_HIST_CAP];
static uint32_t s_l2_ring_head;
static uint32_t s_l2_ring_count;
static dbmap_l2_continuity_t s_l2_continuity;
static dbmap_l2_interval_t s_l2_last_interval;
static bool s_l2_last_interval_valid;
#endif
#endif
#endif
#endif

/*
 * Sequence persistence (Phase 2)
 * ------------------------------
 * NVS key seq_lim holds an exclusive upper bound of a reserved block.
 * RAM hands out numbers in [next, limit). On boot, next starts at limit so any
 * unused remainder of the previous block is skipped (gaps OK).
 *
 * Consumption: when sp_obs_seq_allocate() returns N, N is consumed. The block
 * containing N was already persisted. A failed MQTT publish does not reclaim N.
 *
 * MQTT down: do not allocate; drop the emission; detection continues.
 */

static esp_err_t seq_limit_load(uint32_t *out_limit, uint32_t block_size)
{
    if (out_limit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_limit = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint32_t limit = 0;
    err = nvs_get_u32(h, NVS_KEY_SEQ_LIMIT, &limit);
    if (err == ESP_OK) {
        nvs_close(h);
        *out_limit = limit;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }

    /*
     * Phase 1 key `seq` = next sequence to assign (written to NVS *before*
     * MQTT publish after allocating). Numbers < legacy are consumed.
     * Skip forward one block so we never reuse a number that may have been
     * published in a crash window.
     */
    uint32_t legacy_next = 0;
    err = nvs_get_u32(h, NVS_KEY_SEQ_LEGACY, &legacy_next);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_limit = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *out_limit = sp_obs_seq_migrate_legacy_next(legacy_next, block_size);
    ESP_LOGW(
        TAG,
        "migrated Phase 1 seq=%lu → seq_lim=%lu (skip-forward block=%lu)",
        (unsigned long)legacy_next,
        (unsigned long)*out_limit,
        (unsigned long)(block_size == 0 ? 1 : block_size)
    );
    return ESP_OK;
}

static esp_err_t seq_limit_store(uint32_t limit)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_SEQ_LIMIT, limit);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t seq_ensure_reserved(void)
{
    if (!sp_obs_seq_needs_reserve(&s_seq)) {
        return ESP_OK;
    }
    uint32_t new_limit = sp_obs_seq_reserve(&s_seq);
    esp_err_t err = seq_limit_store(new_limit);
    if (err != ESP_OK) {
        /* Roll back RAM limit so we do not hand out unpersisted numbers. */
        s_seq.limit = s_seq.next;
        return err;
    }
    ESP_LOGI(
        TAG,
        "reserved sequence block next=%lu limit=%lu",
        (unsigned long)s_seq.next,
        (unsigned long)s_seq.limit
    );
    return ESP_OK;
}

static esp_err_t seq_allocate_consumed(uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = seq_ensure_reserved();
    if (err != ESP_OK) {
        return err;
    }
    if (!sp_obs_seq_allocate(&s_seq, out)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void format_uuid_v4(char out[37])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
    snprintf(
        out,
        37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],
        bytes[1],
        bytes[2],
        bytes[3],
        bytes[4],
        bytes[5],
        bytes[6],
        bytes[7],
        bytes[8],
        bytes[9],
        bytes[10],
        bytes[11],
        bytes[12],
        bytes[13],
        bytes[14],
        bytes[15]
    );
}

static esp_err_t format_event_time_for_interval_start(
    int64_t interval_start_mono_us,
    char event_time[40],
    const char **timing_quality,
    double *uncertainty_ms
)
{
    if (event_time == NULL || timing_quality == NULL || uncertainty_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_epoch_us = 0;
    int64_t now_mono = (int64_t)esp_timer_get_time();
    esp_err_t time_err = sp_time_get_epoch_us(&now_epoch_us);
    if (time_err == ESP_OK) {
        int64_t start_epoch_us = now_epoch_us - (now_mono - interval_start_mono_us);
        if (start_epoch_us < 0) {
            start_epoch_us = 0;
        }
        time_t start_sec = (time_t)(start_epoch_us / 1000000LL);
        struct tm utc_time;
        if (gmtime_r(&start_sec, &utc_time) == NULL ||
            strftime(event_time, 40, "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
            return ESP_FAIL;
        }
        *timing_quality = "synchronized";
        *uncertainty_ms = 100.0;
        return ESP_OK;
    }

    time_t now = time(NULL);
    if (now < 1577836800) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t delta_us = now_mono - interval_start_mono_us;
    time_t start_sec = now - (time_t)(delta_us / 1000000LL);
    struct tm utc_time;
    if (gmtime_r(&start_sec, &utc_time) == NULL ||
        strftime(event_time, 40, "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        return ESP_FAIL;
    }
    *timing_quality = "estimated";
    *uncertainty_ms = 5000.0;
    return ESP_OK;
}

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
static ear_p8_outcomes_t s_p8_outcomes;
static ear_runtime_snapshot_t s_p8_snapshot;
static portMUX_TYPE s_p8_lock = portMUX_INITIALIZER_UNLOCKED;

static void p8_snapshot_enter(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_p8_lock);
}

static void p8_snapshot_leave(void *ctx)
{
    (void)ctx;
    portEXIT_CRITICAL(&s_p8_lock);
}

static const ear_runtime_snapshot_lock_t s_p8_snapshot_lock = {
    .enter = p8_snapshot_enter,
    .leave = p8_snapshot_leave,
};

/* Called by the lifecycle owner after all managed callbacks have returned. */
static void observation_capture_runtime_diag(void)
{
    ear_obs_manager_diag_t od;
    ear_promotion_policy_diag_t pd;
    ce_diag_t cd;
    ae_device_diag_t ad;
    sp_mic_window_diag_t md;
    ear_obs_manager_get_diag(&s_obs_mgr, &od);
    ear_promotion_policy_get_diag(&s_promo_pol, &pd);
    ce_get_diag(&s_ce, &cd);
    ae_device_get_diag(&ad);
    sp_mic_get_window_diag(&md);
    const ae_engine_t *ae = ae_device_engine();
    ear_runtime_snapshot_t next = {
        .values = {
            [EAR_RUNTIME_obs_reserve_full] = od.reserve_full,
            [EAR_RUNTIME_obs_promote_commit_fail] = od.promote_commit_fail,
            [EAR_RUNTIME_obs_drain_alloc_fail] = od.drain_alloc_fail,
            [EAR_RUNTIME_obs_pending_timeout] = od.pending_timeout,
            [EAR_RUNTIME_obs_evidence_dropped] = od.evidence_dropped,
            [EAR_RUNTIME_obs_enter_no_rsv] = od.enter_no_rsv,
            [EAR_RUNTIME_obs_policy_reserve_full] = pd.reserve_full,
            [EAR_RUNTIME_obs_policy_promote_fail] = pd.promote_fail,
            [EAR_RUNTIME_ce_create_dropped_full] = cd.create_dropped_full,
            [EAR_RUNTIME_ce_workflow_failed] = cd.workflow_failed,
            [EAR_RUNTIME_ce_module_errors] = cd.module_errors,
            [EAR_RUNTIME_mic_windows_published] = md.published,
            [EAR_RUNTIME_mic_pcm_overwrites] = md.overwrites,
            [EAR_RUNTIME_mic_pcm_pending] = md.pending,
            [EAR_RUNTIME_mic_pcm_projection_failed] = md.projection_failed,
            [EAR_RUNTIME_mic_pcm_reset_discards] = md.reset_discards,
            [EAR_RUNTIME_mic_pcm_legacy_takes] = md.legacy_takes,
            [EAR_RUNTIME_ae_epoch] = ad.epoch,
            [EAR_RUNTIME_pcm_jobs_taken] = ad.pcm_jobs_taken,
            [EAR_RUNTIME_ae_analyze_ok] = ad.analyze_ok,
            [EAR_RUNTIME_ae_analyze_fail] = ad.analyze_fail,
            [EAR_RUNTIME_sig_build_failed] = ad.sig_build_failed,
            [EAR_RUNTIME_sig_publish_rejected] = ae_publish_rejected_count(ae),
            [EAR_RUNTIME_sig_accepted] = ad.sig_accepted,
            [EAR_RUNTIME_ce_in_use] = cd.in_use_count,
            [EAR_RUNTIME_ce_active] = cd.state_hist[AE_CAND_ACTIVE],
            [EAR_RUNTIME_ce_created] = cd.created,
            [EAR_RUNTIME_ce_expired] = cd.expired,
            [EAR_RUNTIME_ce_promote_successes] = cd.promote_successes,
            [EAR_RUNTIME_pol_hold_irrelevant] = pd.hold_irrelevant,
            [EAR_RUNTIME_pol_hold_no_class] = pd.hold_no_class,
            [EAR_RUNTIME_pol_hold_signature_open] = pd.hold_signature_open,
            [EAR_RUNTIME_pol_hold_busy] = pd.promote_skipped_busy,
            [EAR_RUNTIME_pol_select_none] = pd.select_none,
            [EAR_RUNTIME_pol_reserve_ok] = pd.reserve_ok,
            [EAR_RUNTIME_pol_promote_ok] = pd.promote_ok,
            [EAR_RUNTIME_om_open] = ear_obs_manager_n_open(&s_obs_mgr),
            [EAR_RUNTIME_om_pending] = ear_obs_manager_n_pending(&s_obs_mgr),
            [EAR_RUNTIME_om_held] = ear_obs_manager_n_held(&s_obs_mgr),
            [EAR_RUNTIME_om_start_retry] = od.start_retry,
            [EAR_RUNTIME_om_watchdog_end] = od.detached_watchdog_end,
            [EAR_RUNTIME_om_evq_dropped] = od.evq_dropped,
            [EAR_RUNTIME_om_promote_commit_bound] = od.promote_commit_bound,
            [EAR_RUNTIME_om_promote_commit_pending] = od.promote_commit_pending,
            [EAR_RUNTIME_obs_start_attempts] = od.start_attempts,
            [EAR_RUNTIME_obs_periodic_attempts] = od.periodic_attempts,
            [EAR_RUNTIME_obs_end_attempts] = od.end_attempts,
            [EAR_RUNTIME_ear_p8_saturated] = md.saturated || ad.signature_counters_saturated || od.attempts_saturated || s_p8_outcomes.saturated,
            [EAR_RUNTIME_obs_start_publish_accepted] = s_p8_outcomes.count[EAR_P8_START][EAR_P8_PUBLISH_ACCEPTED],
            [EAR_RUNTIME_obs_start_mqtt_publish_failed] = s_p8_outcomes.count[EAR_P8_START][EAR_P8_MQTT_PUBLISH_FAILED],
            [EAR_RUNTIME_obs_start_pre_mqtt_fail] = s_p8_outcomes.count[EAR_P8_START][EAR_P8_PRE_MQTT_FAIL],
            [EAR_RUNTIME_obs_periodic_publish_accepted] = s_p8_outcomes.count[EAR_P8_PERIODIC][EAR_P8_PUBLISH_ACCEPTED],
            [EAR_RUNTIME_obs_periodic_mqtt_publish_failed] = s_p8_outcomes.count[EAR_P8_PERIODIC][EAR_P8_MQTT_PUBLISH_FAILED],
            [EAR_RUNTIME_obs_periodic_pre_mqtt_fail] = s_p8_outcomes.count[EAR_P8_PERIODIC][EAR_P8_PRE_MQTT_FAIL],
            [EAR_RUNTIME_obs_end_publish_accepted] = s_p8_outcomes.count[EAR_P8_END][EAR_P8_PUBLISH_ACCEPTED],
            [EAR_RUNTIME_obs_end_mqtt_publish_failed] = s_p8_outcomes.count[EAR_P8_END][EAR_P8_MQTT_PUBLISH_FAILED],
            [EAR_RUNTIME_obs_end_pre_mqtt_fail] = s_p8_outcomes.count[EAR_P8_END][EAR_P8_PRE_MQTT_FAIL],
        },
    };
    ear_runtime_snapshot_publish(&s_p8_snapshot, &next, &s_p8_snapshot_lock);
}

static void observation_enrich_attach_mgr_ce_diag(cJSON *state)
{
    ear_runtime_snapshot_t snapshot;
    ear_runtime_snapshot_read(&s_p8_snapshot, &snapshot, &s_p8_snapshot_lock);
    ear_runtime_diag_attach(state, &snapshot);
}
#endif /* CONFIG_DBMAP_EAR_AUDIO_ENGINE */

static void observation_enrich(cJSON *state, const sp_effective_t *cfg, bool include_diag)
{
    (void)cfg;
    if (state == NULL) {
        return;
    }

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    if (include_diag) {
        observation_enrich_attach_mgr_ce_diag(state);
    }
#else
    (void)include_diag;
#endif

    cJSON *capabilities = cJSON_GetObjectItem(state, "capabilities");
    if (capabilities == NULL) {
        capabilities = cJSON_CreateObject();
        if (capabilities == NULL) {
            return;
        }
        cJSON_AddItemToObject(state, "capabilities", capabilities);
    }

    cJSON *observation = cJSON_CreateObject();
    if (observation == NULL) {
        return;
    }
    cJSON_AddBoolToObject(observation, "publisher", true);
    cJSON_AddNumberToObject(observation, "protocol_version", 1);
#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
    cJSON_AddStringToObject(observation, "detector_kind", "rms_activity");
    cJSON_AddNumberToObject(observation, "detector_version", 1);
#else
    cJSON_AddStringToObject(observation, "detector_kind", "none");
    cJSON_AddNumberToObject(observation, "detector_version", 0);
#endif
    /* Compiled Relative END v1 settings (CONFIG_DBMAP_EAR_OBS_*); Hub reads via hello/health. */
#if defined(CONFIG_DBMAP_EAR_OBS_RELATIVE_END) && CONFIG_DBMAP_EAR_OBS_RELATIVE_END
    cJSON_AddBoolToObject(observation, "EAR_OBS_RELATIVE_END", true);
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_RETURN_MARGIN_DB_X1000",
        (double)CONFIG_DBMAP_EAR_OBS_REL_RETURN_MARGIN_DB_X1000
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_EXIT_HOLD_MS",
        (double)CONFIG_DBMAP_EAR_OBS_REL_EXIT_HOLD_MS
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_CANCEL_HYST_DB_X1000",
        (double)CONFIG_DBMAP_EAR_OBS_REL_CANCEL_HYST_DB_X1000
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_MIN_CONTRAST_DB_X1000",
        (double)CONFIG_DBMAP_EAR_OBS_REL_MIN_CONTRAST_DB_X1000
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_BASELINE_STALE_MS",
        (double)CONFIG_DBMAP_EAR_OBS_REL_BASELINE_STALE_MS
    );
#if defined(CONFIG_DBMAP_EAR_OBS_REL_LATE_ARM) && CONFIG_DBMAP_EAR_OBS_REL_LATE_ARM
    cJSON_AddBoolToObject(observation, "EAR_OBS_REL_LATE_ARM", true);
#else
    cJSON_AddBoolToObject(observation, "EAR_OBS_REL_LATE_ARM", false);
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT) \
    && CONFIG_DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT
    cJSON_AddBoolToObject(observation, "EAR_OBS_REL_SECONDARY_AMBIENT", true);
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_AMB_JUMP_DB_X1000",
        (double)CONFIG_DBMAP_EAR_OBS_REL_AMB_JUMP_DB_X1000
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_AMB_STABLE_WINDOWS",
        (double)CONFIG_DBMAP_EAR_OBS_REL_AMB_STABLE_WINDOWS
    );
    cJSON_AddNumberToObject(
        observation,
        "EAR_OBS_REL_AMB_STABLE_RANGE_DB_X1000",
        (double)CONFIG_DBMAP_EAR_OBS_REL_AMB_STABLE_RANGE_DB_X1000
    );
#else
    cJSON_AddBoolToObject(observation, "EAR_OBS_REL_SECONDARY_AMBIENT", false);
#endif
#else
    cJSON_AddBoolToObject(observation, "EAR_OBS_RELATIVE_END", false);
    cJSON_AddBoolToObject(observation, "EAR_OBS_REL_LATE_ARM", false);
#endif
#if defined(CONFIG_DBMAP_EAR_AE_SIG_CAPTURE) && CONFIG_DBMAP_EAR_AE_SIG_CAPTURE
    cJSON_AddBoolToObject(observation, "AE_SIG_CAPTURE", true);
#else
    cJSON_AddBoolToObject(observation, "AE_SIG_CAPTURE", false);
#endif
    cJSON_AddItemToObject(capabilities, "observation", observation);
}

uint32_t dbmap_ear_observation_next_sequence(void)
{
    return s_seq.next;
}

uint32_t dbmap_ear_observation_seq_limit(void)
{
    return s_seq.limit;
}

esp_err_t dbmap_ear_observation_publish(
    const sp_effective_t *cfg,
    const char *observation_id,
    bool has_signal_level_dbfs,
    double signal_level_dbfs,
    double interval_duration_ms,
    double episode_duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us,
    const char *emission_kind,
    const char *termination_reason,
    const dbmap_l0_interval_t *measurements,
    const dbmap_l1_interval_t *signature,
    const dbmap_l2_interval_t *dynamics
)
{
    return dbmap_ear_observation_publish_ex(
        cfg,
        observation_id,
        has_signal_level_dbfs,
        signal_level_dbfs,
        interval_duration_ms,
        episode_duration_ms,
        capture_timestamp_monotonic_us,
        interval_start_monotonic_us,
        emission_kind,
        termination_reason,
        measurements,
        signature,
        dynamics,
        NULL,
        NULL
    );
}

static esp_err_t observation_publish_result(
    const sp_effective_t *cfg,
    const char *observation_id,
    bool has_signal_level_dbfs,
    double signal_level_dbfs,
    double interval_duration_ms,
    double episode_duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us,
    const char *emission_kind,
    const char *termination_reason,
    const dbmap_l0_interval_t *measurements,
    const dbmap_l1_interval_t *signature,
    const dbmap_l2_interval_t *dynamics,
    const char *origin,
    const ear_cls_assessment_wire_t *assessment,
    sp_mqtt_publish_result_t *result
)
{
    if (cfg == NULL || cfg->identity.node_id[0] == '\0' || cfg->topics.observation[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (observation_id == NULL || observation_id[0] == '\0' || emission_kind == NULL ||
        measurements == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((has_signal_level_dbfs && signal_level_dbfs > 0.0) || interval_duration_ms < 0.0 ||
        episode_duration_ms < 0.0 || capture_timestamp_monotonic_us < 0 ||
        interval_start_monotonic_us < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sp_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool is_final =
        (strcmp(emission_kind, "end") == 0 || strcmp(emission_kind, "complete") == 0);
    if (is_final) {
        if (termination_reason == NULL) {
            ESP_LOGE(TAG, "termination_reason required for %s", emission_kind);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (termination_reason != NULL) {
        ESP_LOGE(TAG, "termination_reason refuse: only valid on end/complete");
        return ESP_ERR_INVALID_ARG;
    }

    /* Non-final reports refuse incomplete L0 when publishing L0 is required. */
    if (!is_final) {
        if (measurements->history_overflow) {
            ESP_LOGE(TAG, "L0 publish refuse: history overflow (incomplete T1–T6)");
            return ESP_ERR_INVALID_STATE;
        }
#if CONFIG_DBMAP_EAR_OBS_L0
        if (!s_l0_capacity_ok) {
            ESP_LOGE(TAG, "L0 publish refuse: interval exceeds L0 history capacity");
            return ESP_ERR_INVALID_STATE;
        }
#endif
        {
            dbmap_ear_mic_l0_perf_t perf;
            if (dbmap_ear_mic_get_l0_perf(&perf) != ESP_OK || !perf.enabled || !perf.pcm_ready) {
                ESP_LOGE(TAG, "L0 publish refuse: L0 analysis unavailable");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    char event_time[40];
    const char *timing_quality = "unsynchronized";
    double uncertainty_ms = 60000.0;
    esp_err_t time_err = format_event_time_for_interval_start(
        interval_start_monotonic_us,
        event_time,
        &timing_quality,
        &uncertainty_ms
    );
    if (time_err != ESP_OK) {
        ESP_LOGW(TAG, "refuse observation publish: wall clock not available (%s)", esp_err_to_name(time_err));
        return time_err == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : time_err;
    }

    uint32_t sequence = 0;
    esp_err_t seq_err = seq_allocate_consumed(&sequence);
    if (seq_err != ESP_OK) {
        ESP_LOGE(TAG, "sequence allocate failed: %s", esp_err_to_name(seq_err));
        return seq_err;
    }

    dbmap_observation_fields_t fields = {
        .observation_id = observation_id,
        .node_id = cfg->identity.node_id,
        .hub_id = cfg->identity.hub_id,
        .sequence_number = sequence,
        .event_time_utc = event_time,
        .capture_timestamp_monotonic_us = capture_timestamp_monotonic_us,
        .timing_quality = timing_quality,
        .has_clock_offset_ms = false,
        .clock_offset_ms = 0.0,
        .timestamp_uncertainty_ms = uncertainty_ms,
        .interval_duration_ms = interval_duration_ms,
        .episode_duration_ms = episode_duration_ms,
        .emission_kind = emission_kind,
        .termination_reason = termination_reason,
        .origin = origin,
        .has_signal_level_dbfs = has_signal_level_dbfs,
        .signal_level_dbfs = signal_level_dbfs,
        .measurements = measurements,
        .signature = signature,
        .dynamics = dynamics,
        .assessment = (assessment != NULL && assessment->present) ? assessment
                                                                  : NULL,
    };

    const int64_t t_pub0 = esp_timer_get_time();
    char *json = dbmap_observation_build_json(&fields);
    if (json == NULL) {
        ESP_LOGE(TAG, "observation JSON build failed (sequence %lu consumed)", (unsigned long)sequence);
        return ESP_FAIL;
    }

    esp_err_t pub_err = sp_mqtt_publish_json_result(cfg->topics.observation, json, 1, result);
    {
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t_pub0);
        s_r0_publish_n++;
        s_r0_publish_us_last = dt;
        s_r0_publish_us_sum += dt;
        if (dt > s_r0_publish_us_max) {
            s_r0_publish_us_max = dt;
        }
    }
    if (pub_err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "published observation_id=%s sequence=%lu interval_ms=%.0f episode_ms=%.0f kind=%s",
            observation_id,
            (unsigned long)sequence,
            interval_duration_ms,
            episode_duration_ms,
            emission_kind
        );
    } else {
        ESP_LOGW(
            TAG,
            "observation publish failed (%s); id=%s sequence=%lu consumed",
            esp_err_to_name(pub_err),
            observation_id,
            (unsigned long)sequence
        );
    }
    free(json);
    return pub_err;
}

esp_err_t dbmap_ear_observation_publish_ex(
    const sp_effective_t *cfg,
    const char *observation_id,
    bool has_signal_level_dbfs,
    double signal_level_dbfs,
    double interval_duration_ms,
    double episode_duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us,
    const char *emission_kind,
    const char *termination_reason,
    const dbmap_l0_interval_t *measurements,
    const dbmap_l1_interval_t *signature,
    const dbmap_l2_interval_t *dynamics,
    const char *origin,
    const ear_cls_assessment_wire_t *assessment
)
{
    return observation_publish_result(
        cfg, observation_id, has_signal_level_dbfs, signal_level_dbfs,
        interval_duration_ms, episode_duration_ms, capture_timestamp_monotonic_us,
        interval_start_monotonic_us, emission_kind, termination_reason, measurements,
        signature, dynamics, origin, assessment, NULL
    );
}

static const char *emit_kind_name(dbmap_activity_emit_kind_t kind)
{
    switch (kind) {
    case DBMAP_ACTIVITY_EMIT_START:
        return "start";
    case DBMAP_ACTIVITY_EMIT_PERIODIC:
        return "periodic";
    case DBMAP_ACTIVITY_EMIT_END:
        return "end";
    default:
        return "none";
    }
}

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY

#if CONFIG_DBMAP_EAR_OBS_L0
static void l0_ring_push(const dbmap_l0_window_t *window, bool usable)
{
    if (window == NULL || !window->valid) {
        return;
    }
    ear_obs_l0_ring_entry_t *slot = &s_l0_ring[s_l0_ring_head];
    slot->window = *window;
    slot->usable = usable ? 1u : 0u;
    s_l0_ring_head = (s_l0_ring_head + 1u) % EAR_OBS_L0_HIST_CAP;
    if (s_l0_ring_count < EAR_OBS_L0_HIST_CAP) {
        s_l0_ring_count++;
    }
}

static void l0_finalize_interval(
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    const char *kind_name,
    dbmap_l0_interval_t *out_iv
)
{
    if (out_iv == NULL) {
        return;
    }
    memset(out_iv, 0, sizeof(*out_iv));

    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc,
        (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ,
        (uint32_t)CONFIG_SP_MIC_WINDOW_MS,
        0,
        interval_start_mono_us,
        interval_end_mono_us,
        s_l0_hist_rms,
        s_l0_hist_hl,
        s_l0_hist_hl_valid,
        s_l0_hist_t_end,
        s_l0_hist_crest,
        s_l0_hist_crest_ok,
        EAR_OBS_L0_HIST_CAP
    );

    const uint32_t n = s_l0_ring_count;
    const uint32_t start = (s_l0_ring_head + EAR_OBS_L0_HIST_CAP - n) % EAR_OBS_L0_HIST_CAP;
    for (uint32_t i = 0; i < n; ++i) {
        const ear_obs_l0_ring_entry_t *e = &s_l0_ring[(start + i) % EAR_OBS_L0_HIST_CAP];
        if (e->usable == 0) {
            continue;
        }
        dbmap_l0_interval_accum_add(&acc, &e->window);
    }

    dbmap_l0_interval_accum_finalize(&acc, out_iv);
    if (out_iv->history_overflow) {
        s_l0_hist_overflow += 1;
        ESP_LOGW(
            TAG,
            "L0 history overflow kind=%s win=%lu cap=%lu (T1–T6 cleared)",
            kind_name != NULL ? kind_name : "?",
            (unsigned long)out_iv->window_count,
            (unsigned long)EAR_OBS_L0_HIST_CAP
        );
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        /* D3: overflow clears L2 metrics and breaks continuity. */
        l2_continuity_break();
#endif
    }
    s_l0_last_interval = *out_iv;
    s_l0_last_interval_valid = true;
}

#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
static void l1_ring_push(const dbmap_l1_window_t *window, bool usable)
{
    if (window == NULL) {
        return;
    }
    ear_obs_l1_ring_entry_t *slot = &s_l1_ring[s_l1_ring_head];
    slot->window = *window;
    slot->usable = usable ? 1u : 0u;
    s_l1_ring_head = (s_l1_ring_head + 1u) % EAR_OBS_L0_HIST_CAP;
    if (s_l1_ring_count < EAR_OBS_L0_HIST_CAP) {
        s_l1_ring_count++;
    }
}

static void l1_finalize_interval(
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    dbmap_l1_interval_t *out_iv
)
{
    if (out_iv == NULL) {
        return;
    }
    dbmap_l1_interval_accum_t acc;
    dbmap_l1_interval_accum_init(
        &acc,
        (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ,
        (uint32_t)CONFIG_SP_MIC_WINDOW_MS,
        interval_start_mono_us,
        interval_end_mono_us
    );
    const uint32_t n = s_l1_ring_count;
    const uint32_t start = (s_l1_ring_head + EAR_OBS_L0_HIST_CAP - n) % EAR_OBS_L0_HIST_CAP;
    for (uint32_t i = 0; i < n; ++i) {
        const ear_obs_l1_ring_entry_t *e = &s_l1_ring[(start + i) % EAR_OBS_L0_HIST_CAP];
        if (e->usable == 0) {
            /* Still associate for invalid count when timestamp matches. */
            if (interval_start_mono_us < e->window.window_end_mono_us &&
                e->window.window_end_mono_us <= interval_end_mono_us) {
                acc.invalid_window_count++;
            }
            continue;
        }
        dbmap_l1_interval_accum_add(&acc, &e->window);
    }
    dbmap_l1_interval_accum_finalize(&acc, out_iv);
}

#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
static void l2_continuity_break(void)
{
    dbmap_l2_continuity_reset(&s_l2_continuity);
}

static void l2_ring_push(const dbmap_l2_window_t *window, bool usable)
{
    if (window == NULL) {
        return;
    }
    ear_obs_l2_ring_entry_t *slot = &s_l2_ring[s_l2_ring_head];
    slot->window = *window;
    slot->usable = usable ? 1u : 0u;
    s_l2_ring_head = (s_l2_ring_head + 1u) % EAR_OBS_L0_HIST_CAP;
    if (s_l2_ring_count < EAR_OBS_L0_HIST_CAP) {
        s_l2_ring_count++;
    }
}

static void l2_finalize_interval(
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool force_overflow,
    dbmap_l2_interval_t *out_iv
)
{
    if (out_iv == NULL) {
        return;
    }
    dbmap_l2_interval_accum_t acc;
    dbmap_l2_interval_accum_init(
        &acc,
        (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ,
        (uint32_t)CONFIG_SP_MIC_WINDOW_MS,
        interval_start_mono_us,
        interval_end_mono_us
    );
    if (force_overflow) {
        dbmap_l2_interval_accum_mark_overflow(&acc);
        l2_continuity_break();
        dbmap_l2_interval_accum_finalize(&acc, out_iv);
        s_l2_last_interval = *out_iv;
        s_l2_last_interval_valid = true;
        return;
    }

    const uint32_t n = s_l2_ring_count;
    const uint32_t start = (s_l2_ring_head + EAR_OBS_L0_HIST_CAP - n) % EAR_OBS_L0_HIST_CAP;
    for (uint32_t i = 0; i < n; ++i) {
        const ear_obs_l2_ring_entry_t *e = &s_l2_ring[(start + i) % EAR_OBS_L0_HIST_CAP];
        if (!(interval_start_mono_us < e->window.window_end_mono_us &&
              e->window.window_end_mono_us <= interval_end_mono_us)) {
            continue;
        }
        if (e->usable == 0) {
            /* Detector-unusable: same association policy as L1 (invalid count). */
            acc.invalid_window_count++;
            continue;
        }
        dbmap_l2_interval_accum_add(&acc, &e->window);
    }
    dbmap_l2_interval_accum_finalize(&acc, out_iv);
    s_l2_last_interval = *out_iv;
    s_l2_last_interval_valid = true;
}
#endif /* L2 */
#endif /* L1 */

static void l0_on_new_window(const sp_mic_levels_t *levels, bool levels_ok)
{
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    /* AE is the sole PCM taker when enabled; keeps L0/L1/L2 windows updated. */
    if (ae_device_active()) {
        (void)ae_device_poll();
    } else {
        (void)dbmap_ear_mic_l0_poll();
    }
#else
    (void)dbmap_ear_mic_l0_poll();
#endif

    if (!levels_ok || levels == NULL || !levels->valid) {
        return;
    }
    if (levels->window_seq == 0 || levels->window_seq == s_l0_last_window_seq) {
        return;
    }

    dbmap_l0_window_t l0w;
    if (dbmap_ear_mic_get_l0_window(&l0w) != ESP_OK || !l0w.valid) {
        return;
    }
    /* Require L0 end time to match this RMS window (avoids pushing stale L0). */
    if (l0w.window_end_mono_us != levels->measured_at_us) {
        return;
    }

#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    /* Missing analysis window(s): break dominant-frequency continuity. */
    if (s_l0_last_window_seq != 0 &&
        levels->window_seq > s_l0_last_window_seq + 1u) {
        l2_continuity_break();
    }
#endif
    s_l0_last_window_seq = levels->window_seq;

    dbmap_activity_sample_t gate = {
        .has_measurement = true,
        .rms_dbfs = levels->rms_dbfs,
        .window_ms = levels->window_ms,
        .measured_at_us = levels->measured_at_us,
        .window_seq = levels->window_seq,
        .ms_since_i2s_enable = levels->ms_since_i2s_enable,
        .transport_status = levels->transport_status,
        .microphone_status = levels->microphone_status,
    };
    const bool usable = dbmap_activity_sample_usable(&gate);
    l0_ring_push(&l0w, usable);

#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    /* L1 may be invalid while L0 is valid; still store for association/invalid counts. */
    dbmap_l1_window_t l1w;
    if (dbmap_ear_mic_get_l1_window(&l1w) == ESP_OK &&
        l1w.window_end_mono_us == levels->measured_at_us) {
        l1_ring_push(&l1w, usable);
    }
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    dbmap_l2_window_t l2w;
    if (dbmap_ear_mic_get_l2_window(&l2w) == ESP_OK &&
        l2w.window_end_mono_us == levels->measured_at_us) {
        dbmap_l2_continuity_push(
            &s_l2_continuity, (uint32_t)CONFIG_SP_MIC_WINDOW_MS, &l2w
        );
        l2_ring_push(&l2w, usable);
    }
#endif
#endif
}
#endif /* CONFIG_DBMAP_EAR_OBS_L0 */

static const char *termination_reason_for_emission(const dbmap_activity_emission_t *emission)
{
    if (emission == NULL || emission->kind != DBMAP_ACTIVITY_EMIT_END) {
        return NULL;
    }
    switch (emission->termination) {
    case DBMAP_ACTIVITY_TERM_INVALID_HOLD:
        return "invalid_hold";
    case DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT:
        /* Protocol currently allows natural|invalid_hold|ota_reboot|report_rotation.
         * Secondary ambient is diagnostically distinct via end_latch/serial diag. */
        return "natural";
    case DBMAP_ACTIVITY_TERM_NATURAL:
        return "natural";
    default:
        return "natural";
    }
}

static void rms_release_only(void)
{
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    /* Promotion-gated path: manager owns registry release. */
    if (s_obs_mgr_ready) {
        return;
    }
#endif
    if (s_rms_bound) {
        sp_episode_registry_release(&s_registry, s_rms_handle);
        s_rms_bound = false;
    }
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    /* Episode / registry release: break dominant-frequency continuity (D2). */
    l2_continuity_break();
#endif
}

static void rms_release_and_rearm(void)
{
    rms_release_only();
    s_mqtt_episode_rearm_required = true;
}

static void note_reject_for_kind(const char *kind)
{
    if (kind == NULL) {
        return;
    }
    if (strcmp(kind, "start") == 0) {
        sp_diag_note_obs_start_rejected();
    } else if (strcmp(kind, "periodic") == 0) {
        sp_diag_note_obs_periodic_rejected();
    } else if (strcmp(kind, "end") == 0) {
        sp_diag_note_obs_end_rejected();
    } else if (strcmp(kind, "complete") == 0) {
        sp_diag_note_obs_complete_rejected();
    }
}

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
/**
 * Manager-owned publish path: same L0/L1/L2 finalize helpers as the former
 * RMS handle_emission path. Lifecycle ownership stays in ear_obs_manager.
 */
static void mgr_build_levels(
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    const char *kind,
    dbmap_l0_interval_t *out_l0,
    const dbmap_l1_interval_t **out_sig,
    const dbmap_l2_interval_t **out_dyn,
    dbmap_l1_interval_t *l1_storage,
    dbmap_l2_interval_t *l2_storage
)
{
    memset(out_l0, 0, sizeof(*out_l0));
    *out_sig = NULL;
    *out_dyn = NULL;
#if CONFIG_DBMAP_EAR_OBS_L0
    l0_finalize_interval(interval_start_mono_us, interval_end_mono_us, kind, out_l0);
#else
    (void)kind;
    out_l0->sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
    out_l0->analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    l1_finalize_interval(interval_start_mono_us, interval_end_mono_us, l1_storage);
    *out_sig = l1_storage;
#else
    (void)l1_storage;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    l2_finalize_interval(
        interval_start_mono_us,
        interval_end_mono_us,
#if CONFIG_DBMAP_EAR_OBS_L0
        out_l0->history_overflow,
#else
        false,
#endif
        l2_storage
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
    *out_dyn = l2_storage;
#else
    (void)l2_storage;
#endif
#else
    (void)l2_storage;
#endif
}

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE \
    && defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
static bool critical_publish_cb(
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
)
{
    (void)user;
    (void)evidence_or_null;
    if (s_cfg == NULL || observation_id == NULL || emission_kind == NULL) {
        return false;
    }
    if (!sp_mqtt_is_connected()) {
        sp_diag_note_obs_skipped_disconnected();
        return false;
    }
    dbmap_l0_interval_t empty_iv;
    memset(&empty_iv, 0, sizeof(empty_iv));
    empty_iv.sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
    empty_iv.analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;

    const bool is_end =
        (strcmp(emission_kind, "end") == 0 || strcmp(emission_kind, "complete") == 0);
    /*
     * Critical END must not depend on L0 availability. Empty measurements keep
     * the payload schema valid; lifecycle already released the binding.
     */
    esp_err_t err = dbmap_ear_observation_publish_ex(
        s_cfg,
        observation_id,
        false,
        0.0,
        interval_duration_ms,
        episode_duration_ms,
        capture_mono_us,
        interval_start_mono_us,
        emission_kind,
        termination_reason,
        &empty_iv,
        NULL,
        NULL,
        origin,
        NULL
    );
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "critical publish failed kind=%s end=%d err=%s (lifecycle independent)",
            emission_kind,
            is_end ? 1 : 0,
            esp_err_to_name(err)
        );
        return false;
    }
    return true;
}

static void critical_on_ae_signature(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    if (!s_critical_ready || sig == NULL) {
        return;
    }
    ear_critical_on_signature(&s_critical, sig, esp_timer_get_time());
}
#endif /* CRITICAL_PATH */

static void mgr_build_start_aggregates(
    void *ctx,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    ear_obs_start_aggregates_t *out
)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    const dbmap_l1_interval_t *sig = NULL;
    const dbmap_l2_interval_t *dyn = NULL;
    mgr_build_levels(
        interval_start_mono_us,
        capture_mono_us,
        "start",
        &out->l0,
        &sig,
        &dyn,
        &out->l1,
        &out->l2
    );
    if (sig != NULL) {
        out->have_l1 = true;
    }
    if (dyn != NULL) {
        out->have_l2 = true;
    }
}

static void mgr_on_binding_released(
    void *ctx,
    const char *observation_id,
    uint16_t generation
)
{
    (void)ctx;
    ear_obs_start_retain_drop(observation_id, generation);
}

static bool mgr_publish_start(
    sp_mqtt_publish_result_t *result,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    if (s_cfg == NULL || binding == NULL || snap == NULL) {
        return false;
    }
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("start", false);
#endif
    /*
     * Freeze L0/L1/L2 on the first attempt. Retries reuse that aggregate and
     * the capture anchor in now_mono_us. A full retain table does not publish.
     * UTC is resolved later inside publish.
     */
    ear_obs_start_measure_t meas;
    const ear_obs_start_prepare_status_t prepared = ear_obs_start_prepare(
        binding->observation_id,
        binding->ep.generation,
        now_mono_us,
        binding->start_mono_us,
        (int64_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS * 1000LL,
        mgr_build_start_aggregates,
        NULL,
        &meas,
        NULL
    );
    if (prepared != EAR_OBS_START_PREPARE_READY) {
        return false;
    }
    if (!sp_obs_gate_allows_feed(s_emit_gate)) {
        return false;
    }
    if (!sp_mqtt_is_connected()) {
        sp_diag_note_obs_skipped_disconnected();
        return false;
    }
    const int64_t iv_start = meas.interval_start_mono_us;
    const int64_t capture_us = meas.capture_mono_us;
    const double interval_ms = (double)(capture_us - iv_start) / 1000.0;
    bool has_lvl = true;
    double lvl = (double)snap->ema_rms_dbfs;
#if CONFIG_DBMAP_EAR_OBS_L0
    if (meas.l0.has_rms_stats) {
        lvl = (double)meas.l0.rms_mean_dbfs;
    }
#endif
    const ear_cls_assessment_wire_t *assess =
        (binding->assessment.present) ? &binding->assessment : NULL;
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    {
        /*
         * start_anchor emit: START report construction inputs. now_mono_us is
         * the OM-selected START mono from handle_enter (typically
         * last_evidence_mono_us), not esp_timer at publish. Correlate with
         * "start_anchor enter" via cand= / obs=.
         */
        const char *lvl_src = "snap_ema";
#if CONFIG_DBMAP_EAR_OBS_L0
        if (meas.l0.has_rms_stats) {
            lvl_src = "l0_mean";
        }
#endif
#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
        const dbmap_activity_state_t act_st =
            dbmap_activity_detector_state(&s_detector);
        const char *act_name = "unknown";
        switch (act_st) {
        case DBMAP_ACTIVITY_IDLE:
            act_name = "IDLE";
            break;
        case DBMAP_ACTIVITY_PENDING_ENTER:
            act_name = "PENDING_ENTER";
            break;
        case DBMAP_ACTIVITY_ACTIVE:
            act_name = "ACTIVE";
            break;
        case DBMAP_ACTIVITY_PENDING_EXIT:
            act_name = "PENDING_EXIT";
            break;
        default:
            break;
        }
        const int64_t act_last_emit_us = s_detector.last_emit_us;
#else
        const char *act_name = "n/a";
        const int64_t act_last_emit_us = 0;
#endif
        ESP_LOGI(
            TAG,
            "start_anchor emit cand=%llu obs=%s "
            "clock_now_us=%lld selected_start_mono_us=%lld "
            "binding_start_mono_us=%lld iv_start_mono_us=%lld "
            "snap_promote_mono_us=%lld last_evidence_mono_us=%lld "
            "last_window_seq=%lu ema_rms_dbfs=%.2f "
            "selected_level_dbfs=%.2f lvl_src=%s "
            "act_state=%s act_last_emit_us=%lld",
            (unsigned long long)binding->candidate_id,
            binding->observation_id,
            (long long)esp_timer_get_time(),
            (long long)capture_us,
            (long long)binding->start_mono_us,
            (long long)iv_start,
            (long long)snap->promote_mono_us,
            (long long)snap->last_evidence_mono_us,
            (unsigned long)snap->last_window_seq,
            (double)snap->ema_rms_dbfs,
            lvl,
            lvl_src,
            act_name,
            (long long)act_last_emit_us
        );
    }
#endif
    esp_err_t err = observation_publish_result(
        s_cfg,
        binding->observation_id,
        has_lvl,
        lvl,
        interval_ms > 0.0 ? interval_ms : (double)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
        interval_ms > 0.0 ? interval_ms : (double)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
        capture_us,
        iv_start,
        "start",
        NULL,
        &meas.l0,
        meas.have_l1 ? &meas.l1 : NULL,
        meas.have_l2 ? &meas.l2 : NULL,
        NULL,
        assess,
        result
    );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("start", true);
#endif
    if (err != ESP_OK) {
        note_reject_for_kind("start");
        return false;
    }
    ESP_LOGI(
        TAG,
        "obs_mgr START cand=%llu obs=%s",
        (unsigned long long)binding->candidate_id,
        binding->observation_id
    );
    return true;
}

static bool mgr_emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    (void)ctx;
    sp_mqtt_publish_result_t result = {0};
    const bool ok = mgr_publish_start(
        &result, binding, snap, now_mono_us
    );
    ear_p8_note_outcome(&s_p8_outcomes, EAR_P8_START, result.sdk_called, result.message_id);
    return ok;
}

static bool mgr_publish_periodic(
    sp_mqtt_publish_result_t *result,
    const ear_obs_binding_view_t *binding,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    if (s_cfg == NULL || binding == NULL) {
        return false;
    }
    if (!sp_mqtt_is_connected()) {
        sp_diag_note_obs_skipped_disconnected();
        return false;
    }
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("periodic", false);
#endif
    const double interval_ms =
        (double)(interval_end_mono_us - interval_start_mono_us) / 1000.0;
    const double ep_ms =
        (double)(interval_end_mono_us - binding->start_mono_us) / 1000.0;
    dbmap_l0_interval_t local_iv;
    dbmap_l1_interval_t local_l1;
    dbmap_l2_interval_t local_l2;
    const dbmap_l1_interval_t *sig_ptr = NULL;
    const dbmap_l2_interval_t *dyn_ptr = NULL;
    mgr_build_levels(
        interval_start_mono_us,
        interval_end_mono_us,
        "periodic",
        &local_iv,
        &sig_ptr,
        &dyn_ptr,
        &local_l1,
        &local_l2
    );
#if CONFIG_DBMAP_EAR_OBS_L0
    if (local_iv.history_overflow || local_iv.window_count == 0) {
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        ear_obs_stack_hwm_om_publish("periodic", true);
#endif
        ESP_LOGW(TAG, "obs_mgr PERIODIC skipped: invalid L0 (episode stays OPEN)");
        return false;
    }
#endif
    bool has_lvl = has_signal_level_dbfs;
    double lvl = (double)signal_level_dbfs;
#if CONFIG_DBMAP_EAR_OBS_L0
    if (local_iv.has_rms_stats) {
        has_lvl = true;
        lvl = (double)local_iv.rms_mean_dbfs;
    }
#endif
    const ear_cls_assessment_wire_t *assess =
        (binding->assessment.present) ? &binding->assessment : NULL;
    esp_err_t err = observation_publish_result(
        s_cfg,
        binding->observation_id,
        has_lvl,
        lvl,
        interval_ms,
        ep_ms,
        interval_end_mono_us,
        interval_start_mono_us,
        "periodic",
        NULL,
        &local_iv,
        sig_ptr,
        dyn_ptr,
        NULL,
        assess,
        result
    );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("periodic", true);
#endif
    if (err != ESP_OK) {
        note_reject_for_kind("periodic");
        return false;
    }
    return true;
}

static bool mgr_emit_periodic(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    (void)ctx;
    sp_mqtt_publish_result_t result = {0};
    const bool ok = mgr_publish_periodic(
        &result, binding, interval_start_mono_us, interval_end_mono_us,
        has_signal_level_dbfs, signal_level_dbfs
    );
    ear_p8_note_outcome(&s_p8_outcomes, EAR_P8_PERIODIC, result.sdk_called, result.message_id);
    return ok;
}

static bool mgr_publish_end(
    sp_mqtt_publish_result_t *result,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    if (s_cfg == NULL || binding == NULL) {
        return false;
    }
    if (!sp_mqtt_is_connected()) {
        sp_diag_note_obs_skipped_disconnected();
        return false;
    }
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("end", false);
#endif
    const char *term = "natural";
    if (reason == EAR_OBS_END_OTA) {
        term = "ota_reboot";
    }
    const double interval_ms =
        (double)(interval_end_mono_us - interval_start_mono_us) / 1000.0;
    const double ep_ms =
        (double)(interval_end_mono_us - binding->start_mono_us) / 1000.0;
    dbmap_l0_interval_t local_iv;
    dbmap_l1_interval_t local_l1;
    dbmap_l2_interval_t local_l2;
    const dbmap_l1_interval_t *sig_ptr = NULL;
    const dbmap_l2_interval_t *dyn_ptr = NULL;
    mgr_build_levels(
        interval_start_mono_us,
        interval_end_mono_us,
        "end",
        &local_iv,
        &sig_ptr,
        &dyn_ptr,
        &local_l1,
        &local_l2
    );
    bool has_lvl = has_signal_level_dbfs;
    double lvl = (double)signal_level_dbfs;
#if CONFIG_DBMAP_EAR_OBS_L0
    if (local_iv.has_rms_stats) {
        has_lvl = true;
        lvl = (double)local_iv.rms_mean_dbfs;
    }
#endif
    const ear_cls_assessment_wire_t *assess =
        (binding->assessment.present) ? &binding->assessment : NULL;
    esp_err_t err = observation_publish_result(
        s_cfg,
        binding->observation_id,
        has_lvl,
        lvl,
        interval_ms > 0.0 ? interval_ms : (double)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
        ep_ms,
        interval_end_mono_us,
        interval_start_mono_us,
        "end",
        term,
        &local_iv,
        sig_ptr,
        dyn_ptr,
        NULL,
        assess,
        result
    );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_om_publish("end", true);
#endif
    if (err != ESP_OK) {
        note_reject_for_kind("end");
        return false;
    }
    ESP_LOGI(
        TAG,
        "obs_mgr END cand=%llu obs=%s reason=%d",
        (unsigned long long)binding->candidate_id,
        binding->observation_id,
        (int)reason
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    l2_continuity_break();
#endif
    return true;
}

static bool mgr_emit_end(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    (void)ctx;
    sp_mqtt_publish_result_t result = {0};
    const bool ok = mgr_publish_end(
        &result, binding, reason, interval_start_mono_us, interval_end_mono_us,
        has_signal_level_dbfs, signal_level_dbfs
    );
    ear_p8_note_outcome(&s_p8_outcomes, EAR_P8_END, result.sdk_called, result.message_id);
    return ok;
}

#endif /* AUDIO_ENGINE manager emit helpers */

static void handle_emission(const dbmap_activity_emission_t *emission)
{
    if (s_cfg == NULL || emission == NULL || emission->kind == DBMAP_ACTIVITY_EMIT_NONE) {
        return;
    }

    const char *kind = emit_kind_name(emission->kind);
    const bool mqtt_ok = sp_mqtt_is_connected();
    const double interval_ms = emission->duration_ms;

    if (emission->kind == DBMAP_ACTIVITY_EMIT_START) {
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
        /*
         * Promotion-gated path: RMS no longer opens Observations. Activity
         * START is retained only as detector state (priors); Obs create is
         * owned by ear_obs_manager after CE promotion.
         */
        if (s_obs_mgr_ready) {
            ESP_LOGD(TAG, "START ignored: obs_mgr owns Observation create");
            return;
        }
#endif
        if (s_mqtt_episode_rearm_required) {
            ESP_LOGD(TAG, "START suppressed: rearm latch set (need IDLE first)");
            return;
        }
        if (s_rms_bound) {
            ESP_LOGW(TAG, "START while RMS still bound; ignoring");
            return;
        }
        if (!sp_episode_registry_allocate(
                &s_registry,
                &s_rms_handle,
                emission->interval_start_mono_us,
                (uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
                EAR_OBS_PRODUCER_RMS,
                format_uuid_v4,
                false
            )) {
            ESP_LOGW(TAG, "START refused: episode registry full");
            return;
        }
        s_rms_bound = true;
        sp_episode_slot_t *slot = sp_episode_registry_get(&s_registry, s_rms_handle);
        if (slot == NULL) {
            s_rms_bound = false;
            return;
        }

        dbmap_l0_interval_t local_iv;
#if CONFIG_DBMAP_EAR_OBS_L0
        l0_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
            kind,
            &local_iv
        );
#else
        memset(&local_iv, 0, sizeof(local_iv));
        local_iv.sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
        local_iv.analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
        dbmap_l1_interval_t local_l1;
        l1_finalize_interval(
            emission->interval_start_mono_us, emission->interval_end_mono_us, &local_l1
        );
        const dbmap_l1_interval_t *sig_ptr = &local_l1;
#else
        const dbmap_l1_interval_t *sig_ptr = NULL;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        dbmap_l2_interval_t local_l2;
        l2_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
#if CONFIG_DBMAP_EAR_OBS_L0
            local_iv.history_overflow,
#else
            false,
#endif
            &local_l2
        );
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        const dbmap_l2_interval_t *dyn_ptr = &local_l2;
#else
        (void)local_l2;
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
#else
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
        if (!mqtt_ok) {
            s_dropped_while_mqtt_down += 1;
            sp_diag_note_obs_skipped_disconnected();
            rms_release_and_rearm();
            return;
        }
        s_logged_mqtt_drop = false;
        const bool has_lvl = true;
        const double lvl = (double)emission->signal_level_dbfs;
        esp_err_t err = dbmap_ear_observation_publish(
            s_cfg,
            slot->observation_id,
            has_lvl,
            lvl,
            interval_ms,
            interval_ms, /* START: episode duration == first interval */
            emission->interval_end_mono_us,
            emission->interval_start_mono_us,
            "start",
            NULL,
            &local_iv,
            sig_ptr,
            dyn_ptr
        );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        ear_obs_stack_hwm_checkpoint(
            "post_publish",
            "start",
            &s_stack_hwm_bytes_post_publish,
            &s_stack_hwm_have_post_publish
        );
#endif
        if (err == ESP_OK) {
            sp_episode_registry_mark_start_accepted(
                &s_registry, s_rms_handle, emission->interval_end_mono_us
            );
        } else {
            note_reject_for_kind("start");
            rms_release_and_rearm();
        }
        return;
    }

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    if (s_obs_mgr_ready) {
        if (emission->kind == DBMAP_ACTIVITY_EMIT_PERIODIC) {
            /* Preserve RMS interval cadence + L0 window association via hint. */
            ear_obs_manager_on_activity_periodic_hint(
                &s_obs_mgr,
                emission->interval_start_mono_us,
                emission->interval_end_mono_us,
                true,
                emission->signal_level_dbfs
            );
        } else if (emission->kind == DBMAP_ACTIVITY_EMIT_END) {
            /* RMS END → manager hint; INVALID_HOLD still ends AS002. */
            const ear_obs_activity_end_kind_t end_kind =
                (emission->termination == DBMAP_ACTIVITY_TERM_INVALID_HOLD)
                    ? EAR_OBS_ACT_END_INVALID_HOLD
                    : EAR_OBS_ACT_END_AMBIENT;
            ear_obs_manager_on_activity_end_hint(
                &s_obs_mgr,
                end_kind,
                emission->interval_start_mono_us,
                emission->interval_end_mono_us,
                true,
                emission->signal_level_dbfs
            );
        }
        /* START ignored above; Obs create is promotion-gated. */
        return;
    }
#endif

    if (!s_rms_bound) {
        return;
    }
    sp_episode_slot_t *slot = sp_episode_registry_get(&s_registry, s_rms_handle);
    if (slot == NULL || !slot->start_publish_accepted) {
        /* No accepted START → never PERIODIC/END for this binding. */
        rms_release_and_rearm();
        return;
    }

    const double ep_ms =
        (double)(emission->interval_end_mono_us - slot->episode_start_mono_us) / 1000.0;

    if (emission->kind == DBMAP_ACTIVITY_EMIT_PERIODIC) {
        if (!mqtt_ok) {
            s_dropped_while_mqtt_down += 1;
            sp_diag_note_obs_skipped_disconnected();
            /* Keep OPEN; do not advance last_report_end. */
            return;
        }
        dbmap_l0_interval_t local_iv;
#if CONFIG_DBMAP_EAR_OBS_L0
        l0_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
            kind,
            &local_iv
        );
        if (local_iv.history_overflow || local_iv.window_count == 0) {
            ESP_LOGW(TAG, "PERIODIC skipped: invalid L0 (episode stays OPEN)");
            return;
        }
#else
        memset(&local_iv, 0, sizeof(local_iv));
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
        dbmap_l1_interval_t local_l1;
        l1_finalize_interval(
            emission->interval_start_mono_us, emission->interval_end_mono_us, &local_l1
        );
        const dbmap_l1_interval_t *sig_ptr = &local_l1;
#else
        const dbmap_l1_interval_t *sig_ptr = NULL;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        dbmap_l2_interval_t local_l2;
        const bool ov =
#if CONFIG_DBMAP_EAR_OBS_L0
            local_iv.history_overflow;
#else
            false;
#endif
        l2_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
            ov,
            &local_l2
        );
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        const dbmap_l2_interval_t *dyn_ptr = &local_l2;
#else
        (void)local_l2;
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
#else
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
        esp_err_t err = dbmap_ear_observation_publish(
            s_cfg,
            slot->observation_id,
            true,
            (double)emission->signal_level_dbfs,
            interval_ms,
            ep_ms,
            emission->interval_end_mono_us,
            emission->interval_start_mono_us,
            "periodic",
            NULL,
            &local_iv,
            sig_ptr,
            dyn_ptr
        );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        ear_obs_stack_hwm_checkpoint(
            "post_publish",
            "periodic",
            &s_stack_hwm_bytes_post_publish,
            &s_stack_hwm_have_post_publish
        );
#endif
        if (err == ESP_OK) {
            sp_episode_registry_note_periodic_published(
                &s_registry, s_rms_handle, emission->interval_end_mono_us
            );
        } else {
            note_reject_for_kind("periodic");
            /* Keep OPEN; do not advance last_report_end. */
        }
        return;
    }

    if (emission->kind == DBMAP_ACTIVITY_EMIT_END) {
        const char *term = termination_reason_for_emission(emission);
        dbmap_l0_interval_t local_iv;
#if CONFIG_DBMAP_EAR_OBS_L0
        l0_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
            kind,
            &local_iv
        );
#else
        memset(&local_iv, 0, sizeof(local_iv));
        local_iv.sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
        local_iv.analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
        dbmap_l1_interval_t local_l1;
        l1_finalize_interval(
            emission->interval_start_mono_us, emission->interval_end_mono_us, &local_l1
        );
        const dbmap_l1_interval_t *sig_ptr = &local_l1;
#else
        const dbmap_l1_interval_t *sig_ptr = NULL;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        dbmap_l2_interval_t local_l2;
        l2_finalize_interval(
            emission->interval_start_mono_us,
            emission->interval_end_mono_us,
#if CONFIG_DBMAP_EAR_OBS_L0
            local_iv.history_overflow,
#else
            false,
#endif
            &local_l2
        );
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        const dbmap_l2_interval_t *dyn_ptr = &local_l2;
#else
        (void)local_l2;
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
        l2_continuity_break(); /* episode END */
#else
        const dbmap_l2_interval_t *dyn_ptr = NULL;
#endif
        if (!mqtt_ok) {
            s_dropped_while_mqtt_down += 1;
            sp_diag_note_obs_skipped_disconnected();
            rms_release_and_rearm();
            return;
        }
        esp_err_t err = dbmap_ear_observation_publish(
            s_cfg,
            slot->observation_id,
            true,
            (double)emission->signal_level_dbfs,
            interval_ms,
            ep_ms,
            emission->interval_end_mono_us,
            emission->interval_start_mono_us,
            "end",
            term != NULL ? term : "natural",
            &local_iv,
            sig_ptr,
            dyn_ptr
        );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        ear_obs_stack_hwm_checkpoint(
            "post_publish",
            "end",
            &s_stack_hwm_bytes_post_publish,
            &s_stack_hwm_have_post_publish
        );
#endif
        if (err != ESP_OK) {
            note_reject_for_kind("end");
            rms_release_and_rearm();
        } else {
            rms_release_only();
        }
        return;
    }
}

#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
static const char *activity_state_name(dbmap_activity_state_t st)
{
    switch (st) {
    case DBMAP_ACTIVITY_IDLE:
        return "IDLE";
    case DBMAP_ACTIVITY_PENDING_ENTER:
        return "PENDING_ENTER";
    case DBMAP_ACTIVITY_ACTIVE:
        return "ACTIVE";
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return "PENDING_EXIT";
    default:
        return "unknown";
    }
}

/**
 * Rate-limited console diagnostics. Runs only from activity_task (owns
 * s_detector / drop counter). Mic levels are already snapshotted by
 * sp_mic_get_levels under its critical section. Does not block I2S.
 */
static void activity_serial_diag(const sp_mic_levels_t *levels, bool levels_ok)
{
    const int64_t now_us = esp_timer_get_time();

    if (s_diag_last_snap_us == 0 || (now_us - s_diag_last_snap_us) >= 1000000LL) {
        s_diag_last_snap_us = now_us;

        bool usable = false;
        if (levels_ok && levels->valid) {
            dbmap_activity_sample_t gate = {
                .has_measurement = true,
                .rms_dbfs = levels->rms_dbfs,
                .window_ms = levels->window_ms,
                .measured_at_us = levels->measured_at_us,
                .window_seq = levels->window_seq,
                .ms_since_i2s_enable = levels->ms_since_i2s_enable,
                .transport_status = levels->transport_status,
                .microphone_status = levels->microphone_status,
            };
            usable = dbmap_activity_sample_usable(&gate);
        }

        {
            dbmap_activity_diag_t adiag;
            char rel_buf[384];
            dbmap_activity_detector_fill_diag(&s_detector, &adiag);
            if (dbmap_activity_format_rel_diag(&adiag, rel_buf, sizeof(rel_buf)) < 0) {
                rel_buf[0] = '\0';
            }

            if (levels_ok && levels->valid) {
                ESP_LOGI(
                    TAG,
                    "diag snap mono_us=%lld rms_dbfs=%.2f peak_dbfs=%.2f xport=%s mic=%s "
                    "state=%s enter=%.1f exit=%.1f win_valid=1 usable=%d win_seq=%lu",
                    (long long)now_us,
                    (double)levels->rms_dbfs,
                    (double)levels->peak_dbfs,
                    levels->transport_status[0] ? levels->transport_status : "unavailable",
                    levels->microphone_status[0] ? levels->microphone_status : "unavailable",
                    activity_state_name(dbmap_activity_detector_state(&s_detector)),
                    (double)s_detector.cfg.enter_threshold_dbfs,
                    (double)s_detector.cfg.exit_threshold_dbfs,
                    usable ? 1 : 0,
                    (unsigned long)levels->window_seq
                );
            } else {
                ESP_LOGI(
                    TAG,
                    "diag snap mono_us=%lld rms_dbfs=n/a peak_dbfs=n/a xport=n/a mic=n/a "
                    "state=%s enter=%.1f exit=%.1f win_valid=0 usable=0",
                    (long long)now_us,
                    activity_state_name(dbmap_activity_detector_state(&s_detector)),
                    (double)s_detector.cfg.enter_threshold_dbfs,
                    (double)s_detector.cfg.exit_threshold_dbfs
                );
            }
            if (rel_buf[0] != '\0') {
                ESP_LOGI(TAG, "diag rel %s", rel_buf);
            }
        }

#if CONFIG_DBMAP_EAR_OBS_L0
        {
            dbmap_l0_window_t l0w;
            if (dbmap_ear_mic_get_l0_window(&l0w) == ESP_OK && l0w.valid) {
                ESP_LOGI(
                    TAG,
                    "L0 win rms=%.2f peak=%.2f crest=%.2f clip=%d "
                    "B0=%.2f B1=%.2f B2=%.2f B3=%.2f hl=%s%.2f "
                    "n=%lu end_us=%lld",
                    (double)l0w.rms_dbfs,
                    (double)l0w.peak_dbfs,
                    (double)l0w.crest_db,
                    l0w.clipped ? 1 : 0,
                    (double)l0w.band_dbfs[0],
                    (double)l0w.band_dbfs[1],
                    (double)l0w.band_dbfs[2],
                    (double)l0w.band_dbfs[3],
                    l0w.hl_valid ? "" : "n/a ",
                    l0w.hl_valid ? (double)l0w.hl_db : 0.0,
                    (unsigned long)l0w.sample_count,
                    (long long)l0w.window_end_mono_us
                );
            }
        }
#endif
    }

    if (s_diag_last_counters_us == 0 || (now_us - s_diag_last_counters_us) >= 5000000LL) {
        s_diag_last_counters_us = now_us;
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
        ESP_LOGI(
            TAG,
            "diag counters v2 start=%lu periodic=%lu end=%lu invalid=%lu "
            "forced_end_invalid=%lu max_age=%lu sig_end=%lu quiet_end=%lu "
            "mqtt_drop=%lu windows=%lu boot_settle=%lu "
            "v2_feed_n=%lu us_last=%lu us_max=%lu",
            (unsigned long)s_lifecycle_v2.emissions_start,
            (unsigned long)s_lifecycle_v2.emissions_periodic,
            (unsigned long)s_lifecycle_v2.emissions_end,
            (unsigned long)s_lifecycle_v2.windows_invalid,
            (unsigned long)s_lifecycle_v2.forced_ends_invalid,
            (unsigned long)s_lifecycle_v2.ends_max_age,
            (unsigned long)s_lifecycle_v2.ends_signature,
            (unsigned long)s_lifecycle_v2.ends_quiet,
            (unsigned long)s_dropped_while_mqtt_down,
            (unsigned long)s_lifecycle_v2.windows_seen,
            (unsigned long)s_lifecycle_v2.windows_boot_settle,
            (unsigned long)s_lc_v2_feed_count,
            (unsigned long)s_lc_v2_feed_us_last,
            (unsigned long)s_lc_v2_feed_us_max
        );
#else
        ESP_LOGI(
            TAG,
            "diag counters start=%lu periodic=%lu end=%lu invalid=%lu "
            "forced_end_invalid=%lu mqtt_drop=%lu windows=%lu boot_settle=%lu",
            (unsigned long)s_detector.emissions_start,
            (unsigned long)s_detector.emissions_periodic,
            (unsigned long)s_detector.emissions_end,
            (unsigned long)s_detector.windows_invalid,
            (unsigned long)s_detector.forced_ends_invalid,
            (unsigned long)s_dropped_while_mqtt_down,
            (unsigned long)s_detector.windows_seen,
            (unsigned long)s_detector.windows_boot_settle
        );
#endif
#if CONFIG_DBMAP_EAR_OBS_L0
        {
            dbmap_ear_mic_l0_perf_t perf;
            if (dbmap_ear_mic_get_l0_perf(&perf) == ESP_OK) {
                const unsigned long l0_avg = perf.analyze_count > 0u
                    ? (unsigned long)(perf.analyze_us_sum / perf.analyze_count)
                    : 0ul;
                ESP_LOGI(
                    TAG,
                    "L0 perf analyze_n=%lu us_last=%lu us_avg=%lu us_max=%lu "
                    "overwrite=%lu active_mics=%u "
                    "stage_reset=%lu stage_loop=%lu stage_post=%lu "
                    "pcm_cap=%lu pcm_bytes=%lu free_heap_init=%lu free_spiram_init=%lu "
                    "ring=%lu/%lu",
                    (unsigned long)perf.analyze_count,
                    (unsigned long)perf.analyze_us_last,
                    l0_avg,
                    (unsigned long)perf.analyze_us_max,
                    (unsigned long)perf.pending_overwrite,
                    (unsigned)perf.last_active_mic_count,
                    (unsigned long)perf.stage_reset_us,
                    (unsigned long)perf.stage_sample_loop_us,
                    (unsigned long)perf.stage_post_us,
                    (unsigned long)perf.pcm_cap_samples,
                    (unsigned long)perf.pcm_bytes_total,
                    (unsigned long)perf.free_heap_after_init,
                    (unsigned long)perf.free_spiram_after_init,
                    (unsigned long)s_l0_ring_count,
                    (unsigned long)EAR_OBS_L0_HIST_CAP
                );
            }
        }
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
        {
            dbmap_ear_mic_l1_perf_t l1perf;
            if (dbmap_ear_mic_get_l1_perf(&l1perf) == ESP_OK && l1perf.enabled) {
                const unsigned long l1_avg = l1perf.analyze_count > 0u
                    ? (unsigned long)(l1perf.analyze_us_sum / l1perf.analyze_count)
                    : 0ul;
                ESP_LOGI(
                    TAG,
                    "L1 perf analyze_n=%lu us_last=%lu us_avg=%lu us_max=%lu",
                    (unsigned long)l1perf.analyze_count,
                    (unsigned long)l1perf.analyze_us_last,
                    l1_avg,
                    (unsigned long)l1perf.analyze_us_max
                );
            }
        }
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        {
            dbmap_ear_mic_l2_perf_t l2perf;
            if (dbmap_ear_mic_get_l2_perf(&l2perf) == ESP_OK && l2perf.enabled) {
                const unsigned long l2_avg = l2perf.analyze_count > 0u
                    ? (unsigned long)(l2perf.combined_us_sum / l2perf.analyze_count)
                    : 0ul;
                ESP_LOGI(
                    TAG,
                    "L2 perf analyze_n=%lu l1_l2_combined_us last=%lu avg=%lu max=%lu "
                    "l2_incremental_estimated_us last=%lu max=%lu "
                    "(baseline_l1 last=%lu max=%lu) l2_rms_us last=%lu max=%lu "
                    "publish=%d",
                    (unsigned long)l2perf.analyze_count,
                    (unsigned long)l2perf.combined_us_last,
                    l2_avg,
                    (unsigned long)l2perf.combined_us_max,
                    (unsigned long)l2perf.incremental_estimated_us_last,
                    (unsigned long)l2perf.incremental_estimated_us_max,
                    (unsigned long)l2perf.baseline_l1_us_last,
                    (unsigned long)l2perf.baseline_l1_us_max,
                    (unsigned long)l2perf.rms_us_last,
                    (unsigned long)l2perf.rms_us_max,
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
                    1
#else
                    0
#endif
                );
            }
            if (s_l2_last_interval_valid) {
                ESP_LOGI(
                    TAG,
                    "L2 interval win=%lu invalid=%lu overflow=%d "
                    "flux_vc=%lu env_vc=%lu delta_vc=%lu ring=%lu/%lu",
                    (unsigned long)s_l2_last_interval.window_count,
                    (unsigned long)s_l2_last_interval.invalid_window_count,
                    s_l2_last_interval.history_overflow ? 1 : 0,
                    (unsigned long)s_l2_last_interval.spectral_flux_valid_count,
                    (unsigned long)s_l2_last_interval.envelope_cv_valid_count,
                    (unsigned long)s_l2_last_interval.dominant_delta_valid_count,
                    (unsigned long)s_l2_ring_count,
                    (unsigned long)EAR_OBS_L0_HIST_CAP
                );
            }
        }
#endif
#endif
        {
            /* Generic CPU/heap/SPIRAM/stack → shared SP (tag `sp`). */
            sp_diag_log_resources("ear_obs");
            {
                const unsigned long pm_avg = s_r0_promo_mgr_n > 0u
                    ? (unsigned long)(s_r0_promo_mgr_us_sum / s_r0_promo_mgr_n)
                    : 0ul;
                const unsigned long pub_avg = s_r0_publish_n > 0u
                    ? (unsigned long)(s_r0_publish_us_sum / s_r0_publish_n)
                    : 0ul;
                ESP_LOGI(
                    TAG,
                    "R0 stage wall_us promo_mgr n=%lu last=%lu avg=%lu max=%lu "
                    "publish_json_mqtt n=%lu last=%lu avg=%lu max=%lu",
                    (unsigned long)s_r0_promo_mgr_n,
                    (unsigned long)s_r0_promo_mgr_us_last,
                    pm_avg,
                    (unsigned long)s_r0_promo_mgr_us_max,
                    (unsigned long)s_r0_publish_n,
                    (unsigned long)s_r0_publish_us_last,
                    pub_avg,
                    (unsigned long)s_r0_publish_us_max
                );
            }
            /* Phase checkpoints remain Ear-owned (task-phase attribution).
             * Values are FreeRTOS lifetime-min free bytes at the checkpoint;
             * seen=0 means that checkpoint was never reached. */
            ESP_LOGI(
                TAG,
                "stack_hwm_phase_bytes pre_analyze=%lu seen=%d "
                "post_analyze=%lu seen=%d pre_promo=%lu seen=%d "
                "post_promo=%lu seen=%d pre_om_publish=%lu seen=%d "
                "post_om_publish=%lu seen=%d (lifetime-min at checkpoint)",
                s_stack_hwm_have_pre_analyze
                    ? (unsigned long)s_stack_hwm_bytes_pre_analyze
                    : 0ul,
                s_stack_hwm_have_pre_analyze ? 1 : 0,
                s_stack_hwm_have_post_analyze
                    ? (unsigned long)s_stack_hwm_bytes_post_analyze
                    : 0ul,
                s_stack_hwm_have_post_analyze ? 1 : 0,
                s_stack_hwm_have_pre_promo
                    ? (unsigned long)s_stack_hwm_bytes_pre_promo
                    : 0ul,
                s_stack_hwm_have_pre_promo ? 1 : 0,
                s_stack_hwm_have_post_promo
                    ? (unsigned long)s_stack_hwm_bytes_post_promo
                    : 0ul,
                s_stack_hwm_have_post_promo ? 1 : 0,
                s_stack_hwm_have_pre_om_publish
                    ? (unsigned long)s_stack_hwm_bytes_pre_om_publish
                    : 0ul,
                s_stack_hwm_have_pre_om_publish ? 1 : 0,
                s_stack_hwm_have_post_om_publish
                    ? (unsigned long)s_stack_hwm_bytes_post_om_publish
                    : 0ul,
                s_stack_hwm_have_post_om_publish ? 1 : 0
            );
            ESP_LOGI(
                TAG,
                "stack_hwm_phase_bytes pre_ae_diag=%lu seen=%d "
                "post_ae_diag=%lu seen=%d post_publish=%lu seen=%d "
                "post_diag=%lu seen=%d "
                "(lifetime-min; post_publish=legacy RMS path only)",
                s_stack_hwm_have_pre_ae_diag
                    ? (unsigned long)s_stack_hwm_bytes_pre_ae_diag
                    : 0ul,
                s_stack_hwm_have_pre_ae_diag ? 1 : 0,
                s_stack_hwm_have_post_ae_diag
                    ? (unsigned long)s_stack_hwm_bytes_post_ae_diag
                    : 0ul,
                s_stack_hwm_have_post_ae_diag ? 1 : 0,
                s_stack_hwm_have_post_publish
                    ? (unsigned long)s_stack_hwm_bytes_post_publish
                    : 0ul,
                s_stack_hwm_have_post_publish ? 1 : 0,
                s_stack_hwm_have_post_diag
                    ? (unsigned long)s_stack_hwm_bytes_post_diag
                    : 0ul,
                s_stack_hwm_have_post_diag ? 1 : 0
            );
        }
#endif
    }
}
#endif

#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
static void fill_lifecycle_v2_window(
    dbmap_lifecycle_v2_window_t *win,
    const sp_mic_levels_t *levels
)
{
    memset(win, 0, sizeof(*win));
    win->sample.has_measurement = true;
    win->sample.rms_dbfs = levels->rms_dbfs;
    win->sample.window_ms = levels->window_ms;
    win->sample.measured_at_us = levels->measured_at_us;
    win->sample.window_seq = levels->window_seq;
    win->sample.ms_since_i2s_enable = levels->ms_since_i2s_enable;
    win->sample.transport_status = levels->transport_status;
    win->sample.microphone_status = levels->microphone_status;

    dbmap_l0_window_t l0w;
    dbmap_l1_window_t l1w;
    dbmap_l2_window_t l2w;
    const bool have_l0 = (dbmap_ear_mic_get_l0_window(&l0w) == ESP_OK)
                         && l0w.valid
                         && l0w.window_end_mono_us == levels->measured_at_us;
    const bool have_l1 = (dbmap_ear_mic_get_l1_window(&l1w) == ESP_OK)
                         && l1w.window_end_mono_us == levels->measured_at_us;
    const bool have_l2 = (dbmap_ear_mic_get_l2_window(&l2w) == ESP_OK)
                         && l2w.window_end_mono_us == levels->measured_at_us;

    if (have_l0) {
        win->crest_db = l0w.crest_db;
    }
    if (have_l1 && l1w.valid && !l1w.clipped) {
        win->has_flatness = true;
        win->flatness = l1w.flatness;
        win->has_dominant = true;
        win->dominant_hz = l1w.dominant_hz;
    }
    if (have_l2 && l2w.eligible) {
        if (l2w.has_spectral_flux) {
            win->has_spectral_flux = true;
            win->spectral_flux = l2w.spectral_flux;
        }
        if (l2w.has_dominant_delta) {
            win->has_dominant_delta = true;
            win->dominant_delta_hz = l2w.dominant_delta_hz;
        }
    }
    /* Features valid when L0 crest + L1 tonality inputs are present. */
    win->features_valid = have_l0 && win->has_flatness;
}
#endif

static int64_t obs_mono_now(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static void lifecycle_sem_take(void *ctx);
static void lifecycle_sem_give(void *ctx);

typedef struct {
    const sp_mic_levels_t *levels;
    bool levels_ok;
    dbmap_activity_emission_t *emission;
    int64_t poll_t0;
} activity_cycle_t;

static void activity_cycle_before_poll(ear_sig_product_section_args_t *args)
{
    activity_cycle_t *cycle = args->before_ctx;
    const sp_mic_levels_t levels = *cycle->levels;
    const bool levels_ok = cycle->levels_ok;
    dbmap_activity_emission_t emission = *cycle->emission;

    /* Detector and handle_emission run under the lifecycle section. */
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
    /*
     * v2: analyze first so L0/L1/L2 scalars are available for the score,
     * then feed lifecycle. Baseline (v2 off) keeps detector-before-L0 order.
     */
#if CONFIG_DBMAP_EAR_OBS_L0
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_checkpoint(
        "pre_analyze",
        "none",
        &s_stack_hwm_bytes_pre_analyze,
        &s_stack_hwm_have_pre_analyze
    );
#endif
    l0_on_new_window(&levels, levels_ok);
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_checkpoint(
        "post_analyze",
        "none",
        &s_stack_hwm_bytes_post_analyze,
        &s_stack_hwm_have_post_analyze
    );
#endif
#endif
    if (levels_ok && levels.valid) {
        dbmap_lifecycle_v2_window_t win;
        fill_lifecycle_v2_window(&win, &levels);
        const int64_t t0 = esp_timer_get_time();
        emission = dbmap_lifecycle_v2_feed(&s_lifecycle_v2, &win);
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        s_lc_v2_feed_count++;
        s_lc_v2_feed_us_last = dt;
        if (dt > s_lc_v2_feed_us_max) {
            s_lc_v2_feed_us_max = dt;
        }
    }
#else
    if (levels_ok && levels.valid) {
        dbmap_activity_sample_t sample = {
            .has_measurement = true,
            .rms_dbfs = levels.rms_dbfs,
            .peak_dbfs = levels.peak_dbfs,
            .window_ms = levels.window_ms,
            .measured_at_us = levels.measured_at_us,
            .window_seq = levels.window_seq,
            .ms_since_i2s_enable = levels.ms_since_i2s_enable,
            .transport_status = levels.transport_status,
            .microphone_status = levels.microphone_status,
        };
        /* Detector first — L0 must not affect enter/exit decisions. */
        emission = dbmap_activity_detector_feed(&s_detector, &sample);
    }
#if CONFIG_DBMAP_EAR_OBS_L0
    /* Push before finalize so the closing window is in the ring. Membership
     * is start < window_end <= end, so that closing window is associated
     * with this emission (not the next). Still after feed — L0 is not on
     * the detector decision path. */
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    {
        const char *ek = emit_kind_name(emission.kind);
        ear_obs_stack_hwm_checkpoint(
            "pre_analyze",
            ek,
            &s_stack_hwm_bytes_pre_analyze,
            &s_stack_hwm_have_pre_analyze
        );
    }
#endif
    l0_on_new_window(&levels, levels_ok);
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    {
        const char *ek = emit_kind_name(emission.kind);
        ear_obs_stack_hwm_checkpoint(
            "post_analyze",
            ek,
            &s_stack_hwm_bytes_post_analyze,
            &s_stack_hwm_have_post_analyze
        );
    }
#endif
#endif
#endif /* LIFECYCLE_V2 */
    handle_emission(&emission);
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
    if (dbmap_lifecycle_v2_state(&s_lifecycle_v2) == DBMAP_LC_V2_IDLE) {
        s_mqtt_episode_rearm_required = false;
    }
#else
    if (dbmap_activity_detector_state(&s_detector) == DBMAP_ACTIVITY_IDLE) {
        s_mqtt_episode_rearm_required = false;
    }
#endif

    *cycle->emission = emission;
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    if (args->prod != NULL) {
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        ear_obs_stack_hwm_checkpoint(
            "pre_promo",
            "none",
            &s_stack_hwm_bytes_pre_promo,
            &s_stack_hwm_have_pre_promo
        );
#endif
        args->now_mono_us = esp_timer_get_time();
        cycle->poll_t0 = args->now_mono_us;
    }
#endif
}

static void activity_cycle_after_poll(ear_sig_product_section_args_t *args)
{
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    if (args->prod != NULL) {
        observation_capture_runtime_diag();
    }
#endif
    activity_cycle_t *cycle = args->before_ctx;
    if (cycle->poll_t0 == 0) {
        return;
    }
    const uint32_t dt = (uint32_t)(esp_timer_get_time() - cycle->poll_t0);
    s_r0_promo_mgr_n++;
    s_r0_promo_mgr_us_last = dt;
    s_r0_promo_mgr_us_sum += dt;
    if (dt > s_r0_promo_mgr_us_max) {
        s_r0_promo_mgr_us_max = dt;
    }
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ear_obs_stack_hwm_checkpoint(
        "post_promo",
        "none",
        &s_stack_hwm_bytes_post_promo,
        &s_stack_hwm_have_post_promo
    );
#endif
}

static void activity_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RMS activity task started");
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
    ESP_LOGI(TAG, "lifecycle v2 experimental path enabled");
#endif
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ESP_LOGI(TAG, "serial diagnostics enabled (~1 Hz snap, ~5 s counters)");
#endif
#if CONFIG_DBMAP_EAR_OBS_L0
    ESP_LOGI(
        TAG,
        "L0 interval accumulate enabled hist_cap=%lu",
        (unsigned long)EAR_OBS_L0_HIST_CAP
    );
#endif
    while (1) {
        if (!sp_obs_gate_allows_feed(s_emit_gate)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        sp_mic_levels_t levels;
        const bool levels_ok = (sp_mic_get_levels(&levels) == ESP_OK);
        dbmap_activity_emission_t emission;
        memset(&emission, 0, sizeof(emission));
        emission.kind = DBMAP_ACTIVITY_EMIT_NONE;

        activity_cycle_t cycle = {
            .levels = &levels,
            .levels_ok = levels_ok,
            .emission = &emission,
            .poll_t0 = 0,
        };
        ear_sig_product_section_args_t section_args = {
            .prod = NULL,
            .now_mono_us = 0,
            .before_poll = activity_cycle_before_poll,
            .after_poll = activity_cycle_after_poll,
            .before_ctx = &cycle,
        };
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
        if (s_obs_mgr_ready) {
            section_args.prod = &s_sig_product;
        }
#endif
        const ear_obs_lifecycle_lock_ops_t lock_ops = {
            .take = lifecycle_sem_take,
            .give = lifecycle_sem_give,
            .lock_ctx = s_lifecycle_mu,
        };
        /*
         * Detector, START publication, and registry acceptance share this
         * section. Critical Path polling stays after it returns.
         */
        const ear_obs_lifecycle_section_result_t section =
            ear_sig_product_poll_in_section(
                s_lifecycle_mu != NULL ? &lock_ops : NULL,
                &s_emit_gate,
                &section_args
            );
        if (section == EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
        if (s_critical_ready) {
            ear_critical_poll(&s_critical, esp_timer_get_time());
        }
#endif
#if defined(CONFIG_DBMAP_EAR_AE_DIAG) && CONFIG_DBMAP_EAR_AE_DIAG
        /*
         * Obs-manager UART at AE diag cadence (same every-N signature publishes).
         */
        if (s_obs_mgr_ready && ae_device_active()) {
            const uint32_t every = (uint32_t)CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N;
            ae_device_diag_t ad;
            ae_device_get_diag(&ad);
            if (every != 0u && ad.sig_published != 0u
                && (ad.sig_published % every) == 0u
                && ad.sig_published != s_obs_mgr_uart_diag_last_sig) {
                s_obs_mgr_uart_diag_last_sig = ad.sig_published;
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
                ear_obs_stack_hwm_checkpoint(
                    "pre_ae_diag",
                    "none",
                    &s_stack_hwm_bytes_pre_ae_diag,
                    &s_stack_hwm_have_pre_ae_diag
                );
#endif
                ear_obs_manager_diag_t d;
                ear_obs_manager_get_diag(&s_obs_mgr, &d);
                ear_promotion_policy_diag_t pd;
                ear_promotion_policy_get_diag(&s_promo_pol, &pd);
                ESP_LOGI(
                    TAG,
                    "obs_mgr diag sig=%lu open=%u pending=%u held=%u "
                    "rsv_ok=%lu rsv_full=%lu bound=%lu pend_q=%lu "
                    "drain_fail=%lu exit_detach=%lu end=%lu evq_drop=%lu "
                    "promo_cid=%llu decision=%u reason_profile=%u dist=%.3f "
                    "hold_irr=%lu hold_nocls=%lu",
                    (unsigned long)ad.sig_published,
                    (unsigned)ear_obs_manager_n_open(&s_obs_mgr),
                    (unsigned)ear_obs_manager_n_pending(&s_obs_mgr),
                    (unsigned)ear_obs_manager_n_held(&s_obs_mgr),
                    (unsigned long)d.reserve_ok,
                    (unsigned long)d.reserve_full,
                    (unsigned long)d.promote_commit_bound,
                    (unsigned long)d.pending_enqueue,
                    (unsigned long)d.drain_alloc_fail,
                    (unsigned long)d.exit_detach,
                    (unsigned long)d.end_done,
                    (unsigned long)d.evq_dropped,
                    (unsigned long long)pd.last_candidate_id,
                    (unsigned)pd.last_decision,
                    (unsigned)pd.last_profile_id,
                    (double)pd.last_match_distance,
                    (unsigned long)pd.hold_irrelevant,
                    (unsigned long)pd.hold_no_class
                );
                ce_diag_t cd;
                ce_get_diag(&s_ce, &cd);
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
                if (s_critical_ready) {
                    ear_critical_diag_t cpd;
                    ear_critical_get_diag(&s_critical, &cpd);
                    const unsigned long det_n = cpd.lab_trigger;
                    const unsigned long det_avg = det_n > 0u
                        ? (unsigned long)(cpd.lab_detect_us_sum / det_n)
                        : 0ul;
                    const unsigned long prv_avg = det_n > 0u
                        ? (unsigned long)(cpd.lab_preserve_us_sum / det_n)
                        : 0ul;
                    ESP_LOGI(
                        TAG,
                        "critical diag active=%d start=%lu drop_busy=%lu "
                        "auto_blocked=%lu lab_eval=%lu lab_trig=%lu lab_ok=%lu lab_fail=%lu "
                        "detect_us last=%lu avg=%lu max=%lu "
                        "preserve_us last=%lu avg=%lu max=%lu "
                        "force_us last=%lu max=%lu",
                        ear_critical_is_active(&s_critical) ? 1 : 0,
                        (unsigned long)cpd.start,
                        (unsigned long)cpd.drop_busy,
                        (unsigned long)cpd.auto_start_blocked,
                        (unsigned long)cpd.lab_eval,
                        (unsigned long)cpd.lab_trigger,
                        (unsigned long)cpd.lab_start_ok,
                        (unsigned long)cpd.lab_start_fail,
                        (unsigned long)cpd.lab_detect_us_last,
                        det_avg,
                        (unsigned long)cpd.lab_detect_us_max,
                        (unsigned long)cpd.lab_preserve_us_last,
                        prv_avg,
                        (unsigned long)cpd.lab_preserve_us_max,
                        (unsigned long)cpd.lab_force_us_last,
                        (unsigned long)cpd.lab_force_us_max
                    );
                }
#endif
                ESP_LOGI(
                    TAG,
                    "ce diag active=%u created=%lu assoc=%lu expired=%lu rejected=%lu "
                    "promo_ok=%lu gap_seq=%lu gap_q=%lu unusable=%lu "
                    "mod_exec=%lu mod_err=%lu wf_ok=%lu wf_fail=%lu "
                    "keep=%lu skip=%lu promote=%lu reject=%lu err=%lu",
                    (unsigned)cd.in_use_count,
                    (unsigned long)cd.created,
                    (unsigned long)cd.associated,
                    (unsigned long)cd.expired,
                    (unsigned long)cd.rejected,
                    (unsigned long)cd.promote_successes,
                    (unsigned long)cd.seq_discontinuities,
                    (unsigned long)cd.quality_gaps,
                    (unsigned long)cd.ignored_unusable,
                    (unsigned long)cd.module_exec,
                    (unsigned long)cd.module_errors,
                    (unsigned long)cd.workflow_completed,
                    (unsigned long)cd.workflow_failed,
                    (unsigned long)cd.decision_count[CE_DECISION_KEEP],
                    (unsigned long)cd.decision_count[CE_DECISION_SKIP],
                    (unsigned long)cd.decision_count[CE_DECISION_PROMOTE],
                    (unsigned long)cd.decision_count[CE_DECISION_REJECT],
                    (unsigned long)cd.decision_count[CE_DECISION_ERROR]
                );
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
                ear_obs_stack_hwm_checkpoint(
                    "post_ae_diag",
                    "none",
                    &s_stack_hwm_bytes_post_ae_diag,
                    &s_stack_hwm_have_post_ae_diag
                );
#endif
            }
        }
#endif /* AE_DIAG */
#endif /* AUDIO_ENGINE */
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        activity_serial_diag(&levels, levels_ok);
        {
            const char *ek = emit_kind_name(emission.kind);
            ear_obs_stack_hwm_checkpoint(
                "post_diag",
                ek,
                &s_stack_hwm_bytes_post_diag,
                &s_stack_hwm_have_post_diag
            );
        }
#endif
#if CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE
        if (s_cfg != NULL) {
            dbmap_ear_observation_poll_test_trigger(s_cfg);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * Publish a detached OTA END snapshot (stable observation_id from prepare).
 */
static esp_err_t publish_ota_end_snapshot(const ear_obs_end_snapshot_t *snap)
{
    if (snap == NULL || !snap->should_publish) {
        return ESP_OK;
    }
    if (snap->node_id[0] == '\0' || snap->topic[0] == '\0' || snap->observation_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sp_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    dbmap_l0_interval_t empty_iv;
    const dbmap_l0_interval_t *meas = &snap->measurements;
    if (!snap->has_measurements) {
        memset(&empty_iv, 0, sizeof(empty_iv));
        empty_iv.sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
        empty_iv.analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;
        meas = &empty_iv;
    }

    char event_time[40];
    const char *timing_quality = "unsynchronized";
    double uncertainty_ms = 60000.0;
    esp_err_t time_err = format_event_time_for_interval_start(
        snap->interval_start_mono_us,
        event_time,
        &timing_quality,
        &uncertainty_ms
    );
    if (time_err != ESP_OK) {
        return time_err == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : time_err;
    }

    uint32_t sequence = 0;
    esp_err_t seq_err = seq_allocate_consumed(&sequence);
    if (seq_err != ESP_OK) {
        return seq_err;
    }

    dbmap_observation_fields_t fields = {
        .observation_id = snap->observation_id,
        .node_id = snap->node_id,
        .hub_id = snap->hub_id,
        .sequence_number = sequence,
        .event_time_utc = event_time,
        .capture_timestamp_monotonic_us = snap->interval_end_mono_us,
        .timing_quality = timing_quality,
        .has_clock_offset_ms = false,
        .timestamp_uncertainty_ms = uncertainty_ms,
        .interval_duration_ms = snap->interval_duration_ms,
        .episode_duration_ms = snap->episode_duration_ms,
        .emission_kind = snap->emission_kind,
        .termination_reason = snap->termination_reason,
        .origin = NULL,
        .has_signal_level_dbfs = snap->has_signal_level_dbfs,
        .signal_level_dbfs = snap->signal_level_dbfs,
        .measurements = meas,
        .signature = snap->has_signature ? &snap->signature : NULL,
        .dynamics = snap->has_dynamics ? &snap->dynamics : NULL,
    };

    char *json = dbmap_observation_build_json(&fields);
    if (json == NULL) {
        ESP_LOGE(TAG, "OTA END JSON build failed (sequence %lu consumed)", (unsigned long)sequence);
        return ESP_FAIL;
    }

    esp_err_t pub_err =
        sp_mqtt_publish_json_wait(snap->topic, json, 1, kOtaEndAckTimeoutMs);
    free(json);
    return pub_err;
}

typedef struct {
    ear_obs_end_snapshot_t *snaps;
    bool *attempted_end;
} ota_close_args_t;

static void lifecycle_sem_take(void *ctx)
{
    xSemaphoreTake((SemaphoreHandle_t)ctx, portMAX_DELAY);
}

static void lifecycle_sem_give(void *ctx)
{
    xSemaphoreGive((SemaphoreHandle_t)ctx);
}

/*
 * Runs only while s_lifecycle_mu is held by ear_obs_lifecycle_section.
 * Must not take that mutex. END MQTT waits after the section returns.
 */
static void ota_close_locked(void *ctx)
{
    ota_close_args_t *args = (ota_close_args_t *)ctx;
    ear_obs_end_snapshot_t *snaps = args->snaps;
    s_emit_gate = SP_OBS_GATE_CLOSING;

    /* Force detector / v2 idle; registry owns MQTT episode closure. */
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
    (void)dbmap_lifecycle_v2_force_end(&s_lifecycle_v2);
    dbmap_lifecycle_v2_reset_idle(&s_lifecycle_v2);
#else
    (void)dbmap_activity_detector_force_end(&s_detector);
    dbmap_activity_detector_reset_idle(&s_detector);
#endif

    const int64_t now_us = esp_timer_get_time();
    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
        sp_episode_slot_t *slot = &s_registry.slots[i];
        if (!slot->in_use) {
            continue;
        }
        sp_ep_handle_t h = {.index = i, .generation = slot->generation};
        if (!slot->start_publish_accepted) {
            ESP_LOGI(TAG, "OTA close: silent abort slot %u (no accepted START)", (unsigned)i);
            sp_episode_registry_release(&s_registry, h);
            continue;
        }

        ear_obs_end_snapshot_t *snap = &snaps[i];
        snap->should_publish = true;
        *args->attempted_end = true;
        strncpy(snap->observation_id, slot->observation_id, sizeof(snap->observation_id) - 1);
        strncpy(snap->emission_kind, "end", sizeof(snap->emission_kind) - 1);
        strncpy(snap->termination_reason, "ota_reboot", sizeof(snap->termination_reason) - 1);
        snap->interval_start_mono_us = slot->last_report_end_mono_us;
        snap->interval_end_mono_us = now_us;
        snap->interval_duration_ms =
            (double)(now_us - slot->last_report_end_mono_us) / 1000.0;
        snap->episode_duration_ms =
            (double)(now_us - slot->episode_start_mono_us) / 1000.0;
        snap->has_signal_level_dbfs = false;
#if CONFIG_DBMAP_EAR_OBS_L0
        l0_finalize_interval(
            snap->interval_start_mono_us, snap->interval_end_mono_us, "end", &snap->measurements
        );
        snap->has_measurements = true;
        if (snap->measurements.has_rms_stats) {
            snap->has_signal_level_dbfs = true;
            snap->signal_level_dbfs = (double)snap->measurements.rms_mean_dbfs;
        }
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
        l1_finalize_interval(
            snap->interval_start_mono_us, snap->interval_end_mono_us, &snap->signature
        );
        snap->has_signature = true;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        {
            dbmap_l2_interval_t local_l2;
            l2_finalize_interval(
                snap->interval_start_mono_us,
                snap->interval_end_mono_us,
                snap->has_measurements && snap->measurements.history_overflow,
                &local_l2
            );
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
            snap->dynamics = local_l2;
            snap->has_dynamics = true;
#else
            (void)local_l2;
#endif
        }
#endif
        if (s_cfg != NULL) {
            strncpy(snap->node_id, s_cfg->identity.node_id, sizeof(snap->node_id) - 1);
            strncpy(snap->hub_id, s_cfg->identity.hub_id, sizeof(snap->hub_id) - 1);
            strncpy(snap->topic, s_cfg->topics.observation, sizeof(snap->topic) - 1);
        }
        slot->phase = SP_EP_CLOSING;
    }

    s_rms_bound = false;
    s_mqtt_episode_rearm_required = false;
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    /* Registry slot END snapshots are handled above; product bindings next. */
    ear_obs_ota_release_product_bindings(
        s_obs_mgr_ready ? &s_obs_mgr : NULL,
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
        s_critical_ready ? &s_critical : NULL,
#else
        NULL,
#endif
        now_us
    );
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    l2_continuity_break(); /* OTA acoustic close */
#endif
    s_emit_gate = SP_OBS_GATE_CLOSED;
}

static esp_err_t ear_observation_close_for_ota(void)
{
    /* Static: snapshots too large for the main/OTA call stack. */
    static ear_obs_end_snapshot_t snaps[SP_OBS_EPISODE_SLOTS];
    memset(snaps, 0, sizeof(snaps));
    bool attempted_end = false;

    if (!s_started || s_lifecycle_mu == NULL) {
        ESP_LOGI(TAG, "OTA acoustic close: observation not started; nothing to close");
        return ESP_OK;
    }

    ota_close_args_t args = {
        .snaps = snaps,
        .attempted_end = &attempted_end,
    };
    const ear_obs_lifecycle_lock_ops_t ops = {
        .take = lifecycle_sem_take,
        .give = lifecycle_sem_give,
        .lock_ctx = s_lifecycle_mu,
    };
    /*
     * gate NULL: close always runs. Snapshot, cleanup, and the accepted-START
     * decision finish before the give inside ear_obs_lifecycle_section.
     */
    (void)ear_obs_lifecycle_section(&ops, NULL, ota_close_locked, &args);

    const bool mqtt_was_connected = sp_mqtt_is_connected();
    sp_obs_mqtt_wait_result_t wait_result = SP_OBS_MQTT_WAIT_FAILED;
    esp_err_t last_err = ESP_OK;

    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
        ear_obs_end_snapshot_t *snap = &snaps[i];
        if (!snap->should_publish) {
            continue;
        }
        if (!mqtt_was_connected) {
            ESP_LOGW(TAG, "OTA END slot %u skipped: MQTT disconnected", (unsigned)i);
            sp_diag_note_obs_skipped_disconnected();
            wait_result = SP_OBS_MQTT_WAIT_FAILED;
        } else {
            last_err = publish_ota_end_snapshot(snap);
            if (last_err == ESP_OK) {
                wait_result = SP_OBS_MQTT_WAIT_OK;
            } else if (last_err == ESP_ERR_TIMEOUT) {
                wait_result = SP_OBS_MQTT_WAIT_TIMEOUT;
                sp_diag_note_obs_end_rejected();
            } else {
                wait_result = SP_OBS_MQTT_WAIT_FAILED;
                sp_diag_note_obs_end_rejected();
            }
        }
        /* Release corresponding slot after attempt (success or fail). */
        sp_episode_slot_t *slot = &s_registry.slots[i];
        if (slot->in_use && strcmp(slot->observation_id, snap->observation_id) == 0) {
            sp_ep_handle_t h = {.index = i, .generation = slot->generation};
            sp_episode_registry_release(&s_registry, h);
        }
    }

    sp_obs_ota_pub_outcome_t outcome = sp_obs_ota_classify_publish(
        attempted_end, mqtt_was_connected, wait_result
    );
    ESP_LOGI(
        TAG,
        "OTA acoustic close done outcome=%s (PUBACK != Hub ingest)",
        sp_obs_ota_pub_outcome_name(outcome)
    );
    (void)last_err;
    return ESP_OK;
}
#endif /* CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY — activity_task + close helpers */

#if !CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static esp_err_t ear_observation_close_for_ota(void)
{
    ESP_LOGI(TAG, "OTA acoustic close: RMS activity disabled; nothing to close");
    return ESP_OK;
}
#endif

esp_err_t dbmap_ear_observation_register_ota_close(void)
{
    if (s_ota_close_registered) {
        return ESP_OK;
    }
    if (s_lifecycle_mu == NULL) {
        s_lifecycle_mu = xSemaphoreCreateMutex();
        if (s_lifecycle_mu == NULL) {
            ESP_LOGE(TAG, "OTA close register failed: no mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = sp_ota_register_acoustic_close(ear_observation_close_for_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA close register failed: %s", esp_err_to_name(err));
        return err;
    }
    s_emit_gate = SP_OBS_GATE_RUN;
    s_ota_close_registered = true;
    ESP_LOGI(TAG, "OTA acoustic-close callback registered");
    return ESP_OK;
}

esp_err_t dbmap_ear_observation_start(const sp_effective_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }

    uint32_t block_size = (uint32_t)CONFIG_DBMAP_EAR_OBS_SEQ_BLOCK_SIZE;
    uint32_t durable_limit = 0;
    esp_err_t err = seq_limit_load(&durable_limit, block_size);
    if (err != ESP_OK) {
        /* Never treat unexpected NVS failure as a fresh device (seq=0). */
        ESP_LOGE(TAG, "sequence limit load failed (%s); refusing start", esp_err_to_name(err));
        return err;
    }

    sp_obs_seq_init(&s_seq, durable_limit, block_size);
    err = seq_ensure_reserved();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial sequence reserve failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cfg = cfg;
    /* EAR classifier identity for Device Report (Platform emits the field). */
    sig_engine_apply_classifier_version(
        ((sp_effective_t *)cfg)->config.classifier_version,
        sizeof(((sp_effective_t *)cfg)->config.classifier_version)
    );
    sp_health_add_enricher(observation_enrich);

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    err = ae_device_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio Engine device start failed: %s", esp_err_to_name(err));
        return err;
    }
    {
        /*
         * Signature Engine before CE on the AE listener list so primary
         * annotation sees the current window's published results.
         */
        if (!sig_engine_start()) {
            ESP_LOGE(TAG, "Signature Engine start failed");
            ae_device_stop();
            return ESP_FAIL;
        }
        ae_engine_t *ae = ae_device_engine();
        memset(&s_sig_product, 0, sizeof(s_sig_product));
        s_sig_product.om = &s_obs_mgr;
        s_sig_product.policy = &s_promo_pol;
        s_sig_product.ce = &s_ce;
        s_sig_product.ready = false;
        s_sig_engine_listener_bound = false;
        if (ae == NULL
            || !ae_add_signature_listener(
                ae, ear_sig_product_on_ae_signature, &s_sig_product
            )) {
            ESP_LOGE(TAG, "Signature Engine AE listener registration failed");
            sig_engine_stop();
            ae_device_stop();
            return ESP_FAIL;
        }
        s_sig_engine_listener_bound = true;

        ce_config_t ccfg;
        ce_config_set_defaults(&ccfg);
        s_obs_mgr_ready = false;
        s_sig_product.ready = false;
        if (!ce_start(&s_ce, &ccfg)) {
            ESP_LOGE(TAG, "Candidate Engine start failed");
            (void)ae_remove_signature_listener(
                ae, ear_sig_product_on_ae_signature, &s_sig_product
            );
            s_sig_engine_listener_bound = false;
            sig_engine_stop();
            ae_device_stop();
            return ESP_FAIL;
        }
        if (!ce_bind_audio_engine(&s_ce, ae)) {
            ESP_LOGE(TAG, "CE signature listener registration failed");
            ce_stop(&s_ce);
            (void)ae_remove_signature_listener(
                ae, ear_sig_product_on_ae_signature, &s_sig_product
            );
            s_sig_engine_listener_bound = false;
            sig_engine_stop();
            ae_device_stop();
            return ESP_FAIL;
        }
        ESP_LOGI(
            TAG,
            "Signature Engine + CE AE listeners ready (engine before CE); clf=%s",
            sig_engine_classifier_version()
        );
        (void)ae;
    }
#endif

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
    dbmap_activity_config_t act_cfg = {
        .enter_threshold_dbfs = ((float)CONFIG_DBMAP_EAR_OBS_ENTER_THRESHOLD_DBFS_X1000) / 1000.0f,
        .exit_threshold_dbfs = ((float)CONFIG_DBMAP_EAR_OBS_EXIT_THRESHOLD_DBFS_X1000) / 1000.0f,
        .min_activity_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_MIN_ACTIVITY_MS,
        .exit_hold_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_EXIT_HOLD_MS,
        .boot_settle_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_BOOT_SETTLE_MS,
        .observation_interval_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
        .max_invalid_hold_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_MAX_INVALID_HOLD_MS,
#if defined(CONFIG_DBMAP_EAR_OBS_RELATIVE_END) && CONFIG_DBMAP_EAR_OBS_RELATIVE_END
        .relative_end_enabled = true,
        .return_margin_db =
            ((float)CONFIG_DBMAP_EAR_OBS_REL_RETURN_MARGIN_DB_X1000) / 1000.0f,
        .cancel_hysteresis_db =
            ((float)CONFIG_DBMAP_EAR_OBS_REL_CANCEL_HYST_DB_X1000) / 1000.0f,
        .relative_exit_hold_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_REL_EXIT_HOLD_MS,
        .min_event_contrast_db =
            ((float)CONFIG_DBMAP_EAR_OBS_REL_MIN_CONTRAST_DB_X1000) / 1000.0f,
        .baseline_stale_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_REL_BASELINE_STALE_MS,
#if defined(CONFIG_DBMAP_EAR_OBS_REL_LATE_ARM) && CONFIG_DBMAP_EAR_OBS_REL_LATE_ARM
        .relative_end_late_arm_enabled = true,
#else
        .relative_end_late_arm_enabled = false,
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT) \
    && CONFIG_DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT
        .secondary_ambient_enabled = true,
        .ambient_jump_db =
            ((float)CONFIG_DBMAP_EAR_OBS_REL_AMB_JUMP_DB_X1000) / 1000.0f,
        .ambient_stable_windows = (uint32_t)CONFIG_DBMAP_EAR_OBS_REL_AMB_STABLE_WINDOWS,
        .ambient_stable_range_db =
            ((float)CONFIG_DBMAP_EAR_OBS_REL_AMB_STABLE_RANGE_DB_X1000) / 1000.0f,
#else
        .secondary_ambient_enabled = false,
#endif
#else
        .relative_end_enabled = false,
        .relative_end_late_arm_enabled = false,
        .secondary_ambient_enabled = false,
#endif
    };
    if (!dbmap_activity_config_valid(&act_cfg)) {
        ESP_LOGE(TAG, "invalid RMS activity config (check enter > exit thresholds)");
        return ESP_ERR_INVALID_ARG;
    }
    dbmap_activity_detector_init(&s_detector, &act_cfg);
#if defined(CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2) && CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2
    {
        dbmap_lifecycle_v2_config_t v2cfg;
        dbmap_lifecycle_v2_config_set_defaults(&v2cfg);
        v2cfg.gate_enter_dbfs = act_cfg.enter_threshold_dbfs;
        v2cfg.quiet_fallback_dbfs = act_cfg.exit_threshold_dbfs;
        v2cfg.min_activity_ms = act_cfg.min_activity_ms;
        v2cfg.boot_settle_ms = act_cfg.boot_settle_ms;
        v2cfg.observation_interval_ms = act_cfg.observation_interval_ms;
        v2cfg.max_invalid_hold_ms = act_cfg.max_invalid_hold_ms;
        v2cfg.pre_trigger_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_LC_V2_PRE_TRIGGER_MS;
        v2cfg.max_episode_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_LC_V2_MAX_EPISODE_MS;
        v2cfg.score_enter =
            ((float)CONFIG_DBMAP_EAR_OBS_LC_V2_SCORE_ENTER_X1000) / 1000.0f;
        v2cfg.score_exit =
            ((float)CONFIG_DBMAP_EAR_OBS_LC_V2_SCORE_EXIT_X1000) / 1000.0f;
        v2cfg.persistence_windows =
            (uint32_t)CONFIG_DBMAP_EAR_OBS_LC_V2_PERSISTENCE_WINDOWS;
        v2cfg.signature_exit_hold_ms =
            (uint32_t)CONFIG_DBMAP_EAR_OBS_LC_V2_SIGNATURE_EXIT_HOLD_MS;
        v2cfg.quiet_exit_hold_ms =
            (uint32_t)CONFIG_DBMAP_EAR_OBS_LC_V2_QUIET_EXIT_HOLD_MS;
        if (!dbmap_lifecycle_v2_config_valid(&v2cfg)) {
            ESP_LOGE(TAG, "invalid lifecycle v2 config");
            return ESP_ERR_INVALID_ARG;
        }
        dbmap_lifecycle_v2_init(&s_lifecycle_v2, &v2cfg);
        s_lc_v2_feed_us_last = 0;
        s_lc_v2_feed_us_max = 0;
        s_lc_v2_feed_count = 0;
        ESP_LOGI(
            TAG,
            "lifecycle v2 ready pre_trigger_ms=%lu max_episode_ms=%lu "
            "score_enter=%.3f score_exit=%.3f persist=%lu",
            (unsigned long)v2cfg.pre_trigger_ms,
            (unsigned long)v2cfg.max_episode_ms,
            (double)v2cfg.score_enter,
            (double)v2cfg.score_exit,
            (unsigned long)v2cfg.persistence_windows
        );
    }
#endif
    sp_episode_registry_init(&s_registry);
    s_rms_bound = false;
    s_mqtt_episode_rearm_required = false;
#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE
    {
        ear_obs_manager_config_t mcfg;
        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.registry = &s_registry;
        mcfg.ce = &s_ce;
        mcfg.ae = ae_device_engine();
        mcfg.uuid_fn = format_uuid_v4;
        mcfg.observation_interval_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS;
        mcfg.pending_timeout_ms = EAR_OBS_PENDING_TIMEOUT_MS_DEFAULT;
        mcfg.producer_tag = EAR_OBS_PRODUCER_MGR;
        mcfg.emit_start = mgr_emit_start;
        mcfg.emit_periodic = mgr_emit_periodic;
        mcfg.emit_end = mgr_emit_end;
        mcfg.emit_ctx = NULL;
        mcfg.on_binding_released = mgr_on_binding_released;
        mcfg.mono_now_us = obs_mono_now;
        /* PERIODIC driven by RMS activity hints (L0 window cadence). */
        mcfg.enable_poll_periodic = false;
#if defined(CONFIG_DBMAP_EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS)
        mcfg.as002_signature_absence_hold_ms =
            (uint32_t)CONFIG_DBMAP_EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS;
#else
        mcfg.as002_signature_absence_hold_ms =
            EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT;
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_DETACHED_END_TIMEOUT_MS)
        mcfg.detached_end_timeout_ms =
            (uint32_t)CONFIG_DBMAP_EAR_OBS_DETACHED_END_TIMEOUT_MS;
#else
        mcfg.detached_end_timeout_ms = EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT;
#endif
        ear_obs_manager_init(&s_obs_mgr, &mcfg);
        ear_promotion_policy_init(&s_promo_pol, &s_ce, &s_obs_mgr);
        ear_promotion_policy_use_sig_engine_as002(&s_promo_pol, true);
        ce_set_promotion_cb(&s_ce, ear_obs_manager_on_promotion, &s_obs_mgr);
        s_obs_mgr_ready = true;
        s_sig_product.ready = true;
        ESP_LOGI(
            TAG,
            "EAR obs manager ready (promotion-gated; RMS not Obs authority; "
            "sig_engine_listener=%d)",
            s_sig_engine_listener_bound ? 1 : 0
        );
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH) && CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH
        {
            ear_critical_config_t ccfg;
            ear_critical_config_set_defaults(&ccfg);
            ccfg.periodic_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_CRITICAL_PERIODIC_MS;
            ccfg.idle_timeout_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_CRITICAL_IDLE_TIMEOUT_MS;
            ccfg.max_active_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_CRITICAL_MAX_ACTIVE_MS;
            ccfg.match_distance_max =
                ((float)CONFIG_DBMAP_EAR_OBS_CRITICAL_MATCH_DISTANCE_X1000) / 1000.0f;
            ccfg.match_crest_floor_db =
                ((float)CONFIG_DBMAP_EAR_OBS_CRITICAL_MATCH_CREST_FLOOR_DB_X1000) / 1000.0f;
            /* Production auto acoustic START is not implemented; keep false. */
            ccfg.auto_detect_enabled = false;
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_AUTO_DETECT) \
    && CONFIG_DBMAP_EAR_OBS_CRITICAL_AUTO_DETECT
            ESP_LOGW(
                TAG,
                "CRITICAL_AUTO_DETECT is set but no production trigger exists; "
                "acoustic START remains disabled"
            );
#endif
#if defined(CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE) \
    && CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE
            ccfg.lab_impulse_enabled = true;
            ccfg.lab_crest_floor_db =
                ((float)CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_CREST_FLOOR_DB_X1000) / 1000.0f;
            ccfg.lab_peak_dbfs_min =
                ((float)CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_PEAK_DBFS_X1000) / 1000.0f;
            ESP_LOGW(
                TAG,
                "CRITICAL LAB impulse START enabled (measurement only) "
                "crest>=%.2f peak>=%.2f — not production detection",
                (double)ccfg.lab_crest_floor_db,
                (double)ccfg.lab_peak_dbfs_min
            );
#else
            ccfg.lab_impulse_enabled = false;
#endif
            ccfg.publish = critical_publish_cb;
            ccfg.publish_user = NULL;
            ccfg.uuid_fn = format_uuid_v4;
            s_critical_ready = ear_critical_init(&s_critical, &ccfg);
            s_critical_listener_bound = false;
            if (s_critical_ready && mcfg.ae != NULL) {
                s_critical_listener_bound =
                    ae_add_signature_listener(mcfg.ae, critical_on_ae_signature, NULL);
            }
            ESP_LOGI(
                TAG,
                "Critical Path ready auto_detect=off lab_impulse=%d idle_ms=%lu max_ms=%lu "
                "periodic_ms=%lu listener=%d (experimental match D=%.2f crest>=%.2f)",
                ccfg.lab_impulse_enabled ? 1 : 0,
                (unsigned long)ccfg.idle_timeout_ms,
                (unsigned long)ccfg.max_active_ms,
                (unsigned long)ccfg.periodic_ms,
                s_critical_listener_bound ? 1 : 0,
                (double)ccfg.match_distance_max,
                (double)ccfg.match_crest_floor_db
            );
        }
#endif
    }
#endif
#if CONFIG_DBMAP_EAR_OBS_L0
    s_l0_ring_head = 0;
    s_l0_ring_count = 0;
    s_l0_last_window_seq = 0;
    s_l0_last_interval_valid = false;
    s_l0_hist_overflow = 0;
    memset(&s_l0_last_interval, 0, sizeof(s_l0_last_interval));
    s_l0_capacity_ok = (EAR_OBS_L0_WINDOWS_NEEDED <= EAR_OBS_L0_HIST_CAP);
    if (!s_l0_capacity_ok) {
        ESP_LOGE(
            TAG,
            "L0 capacity too small: need %lu windows for interval_ms=%d window_ms=%d, have cap=%lu. "
            "Raise DBMAP_EAR_OBS_L0_MAX_WINDOWS or lower INTERVAL_MS.",
            (unsigned long)EAR_OBS_L0_WINDOWS_NEEDED,
            CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
            CONFIG_SP_MIC_WINDOW_MS,
            (unsigned long)EAR_OBS_L0_HIST_CAP
        );
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
        return ESP_ERR_INVALID_SIZE;
#endif
    } else {
        ESP_LOGI(
            TAG,
            "L0 capacity ok need=%lu cap=%lu (~%lu bytes ring)",
            (unsigned long)EAR_OBS_L0_WINDOWS_NEEDED,
            (unsigned long)EAR_OBS_L0_HIST_CAP,
            (unsigned long)(EAR_OBS_L0_HIST_CAP * sizeof(ear_obs_l0_ring_entry_t))
        );
    }
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    s_l1_ring_head = 0;
    s_l1_ring_count = 0;
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    s_l2_ring_head = 0;
    s_l2_ring_count = 0;
    s_l2_last_interval_valid = false;
    memset(&s_l2_last_interval, 0, sizeof(s_l2_last_interval));
    dbmap_l2_continuity_reset(&s_l2_continuity);
    ESP_LOGI(
        TAG,
        "L2 dynamics compute=1 publish=%d hist_cap=%lu",
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        1,
#else
        0,
#endif
        (unsigned long)EAR_OBS_L0_HIST_CAP
    );
#endif
#endif
#endif
    /*
     * Stack depth is bytes on ESP-IDF 5.4.4 / Xtensa (StackType_t = uint8_t).
     * 8192 = prior 6144 + 2048 for AE/OM/diag margin under active acoustic load.
     */
    TaskHandle_t obs_task = NULL;
    BaseType_t ok = xTaskCreate(activity_task, "ear_obs", 8192, NULL, 4, &obs_task);
    if (ok == pdPASS && obs_task != NULL) {
        sp_diag_register_task("ear_obs", obs_task);
    }
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        TAG,
        "RMS activity detector ready enter=%.1f exit=%.1f min_ms=%lu interval_ms=%lu "
        "relative_end=%d late_arm=%d",
        (double)act_cfg.enter_threshold_dbfs,
        (double)act_cfg.exit_threshold_dbfs,
        (unsigned long)act_cfg.min_activity_ms,
        (unsigned long)act_cfg.observation_interval_ms,
        act_cfg.relative_end_enabled ? 1 : 0,
        act_cfg.relative_end_late_arm_enabled ? 1 : 0
    );
#endif

    s_started = true;
    ESP_LOGI(TAG, "observation protocol v1 hierarchical (episode registry capacity=%d)", SP_OBS_EPISODE_SLOTS);
    ESP_LOGI(
        TAG,
        "observation publisher ready next_seq=%lu seq_limit=%lu block=%lu",
        (unsigned long)s_seq.next,
        (unsigned long)s_seq.limit,
        (unsigned long)s_seq.block_size
    );
    return ESP_OK;
}

void dbmap_ear_observation_poll_test_trigger(const sp_effective_t *cfg)
{
#if !CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE
    (void)cfg;
    return;
#else
    if (cfg == NULL || s_test_published || !s_started) {
        return;
    }
    if (!sp_mqtt_is_connected()) {
        if (!s_logged_wait_mqtt) {
            ESP_LOGI(TAG, "Phase 1 test Observation waiting for MQTT");
            s_logged_wait_mqtt = true;
        }
        return;
    }

    double signal_level_dbfs = ((double)CONFIG_DBMAP_EAR_OBS_TEST_SIGNAL_LEVEL_DBFS) / 1000.0;
    double duration_ms = (double)CONFIG_DBMAP_EAR_OBS_TEST_DURATION_MS;
    int64_t capture_us = (int64_t)esp_timer_get_time();
    int64_t start_us = capture_us;

    sp_mic_levels_t levels;
    if (sp_mic_get_levels(&levels) == ESP_OK && levels.valid) {
        signal_level_dbfs = (double)levels.rms_dbfs;
        if (signal_level_dbfs > 0.0) {
            signal_level_dbfs = 0.0;
        }
        duration_ms = (double)levels.window_ms;
        if (levels.measured_at_us > 0) {
            capture_us = levels.measured_at_us;
            start_us = levels.measured_at_us - ((int64_t)levels.window_ms * 1000LL);
            if (start_us < 0) {
                start_us = 0;
            }
        }
    }

    char oid[37];
    format_uuid_v4(oid);
    dbmap_l0_interval_t empty_iv;
    memset(&empty_iv, 0, sizeof(empty_iv));
    empty_iv.sample_rate_hz = (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ;
    empty_iv.analysis_window_ms = (uint32_t)CONFIG_SP_MIC_WINDOW_MS;
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    dbmap_l1_interval_t empty_l1;
    dbmap_l1_interval_set_empty(
        &empty_l1,
        (uint32_t)CONFIG_SP_MIC_SAMPLE_RATE_HZ,
        (uint32_t)CONFIG_SP_MIC_WINDOW_MS,
        start_us,
        capture_us
    );
    const dbmap_l1_interval_t *sig_ptr = &empty_l1;
#else
    const dbmap_l1_interval_t *sig_ptr = NULL;
#endif
    /* Phase 1 one-shot does not accumulate L2; publish null dynamics. */
    const dbmap_l2_interval_t *dyn_ptr = NULL;
    esp_err_t err = dbmap_ear_observation_publish(
        cfg,
        oid,
        true,
        signal_level_dbfs,
        duration_ms,
        duration_ms,
        capture_us,
        start_us,
        "complete",
        "natural",
        &empty_iv,
        sig_ptr,
        dyn_ptr
    );
    if (err == ESP_OK) {
        s_test_published = true;
        ESP_LOGI(TAG, "Phase 1 test Observation published (one-shot complete)");
    } else if (!s_logged_wait_mqtt) {
        s_logged_wait_mqtt = true;
        ESP_LOGW(TAG, "Phase 1 test Observation publish deferred: %s", esp_err_to_name(err));
    }
#endif
}
