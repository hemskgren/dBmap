#include "sp_connectivity.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sp_lifecycle.h"
#include "sp_mqtt.h"
#include "sp_net_dispatch.h"
#include "sp_wifi.h"

static const char *TAG = "sp_conn";

static TaskHandle_t s_task;
static sp_conn_sm_t s_sm;
static sp_conn_s3_sm_t s_s3;
static sp_conn_s4_sm_t s_s4;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_s4_reboot_armed;
static bool s_mqtt_connected;
static bool s_wifi_has_ip = true;
static bool s_timer_arm_failed;
static sp_conn_fail_t s_timer_fail = SP_CONN_FAIL_NONE;
static sp_conn_status_t s_last_status;
static int64_t s_started_ms;
static int64_t (*s_progress_age_fn)(int64_t now_ms);
static bool (*s_progress_expired_fn)(int64_t now_ms);
static int64_t (*s_mqtt_next_attempt_fn)(int64_t now_ms);
static int64_t (*s_last_verified_fn)(void);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t age_of(int64_t stamp, int64_t now)
{
    if (stamp <= 0) {
        return -1;
    }
    return now - stamp;
}

void sp_connectivity_note_timer_arm_failed(sp_conn_fail_t reason)
{
    portENTER_CRITICAL(&s_lock);
    s_timer_arm_failed = true;
    s_timer_fail = reason;
    s_sm.last_fail = reason;
    portEXIT_CRITICAL(&s_lock);
}

void sp_connectivity_note_timer_arm_ok(void)
{
    portENTER_CRITICAL(&s_lock);
    s_timer_arm_failed = false;
    sp_conn_sm_clear_timer_retries(&s_sm);
    portEXIT_CRITICAL(&s_lock);
}

void sp_connectivity_note_mqtt_connected(bool connected)
{
    portENTER_CRITICAL(&s_lock);
    s_mqtt_connected = connected;
    portEXIT_CRITICAL(&s_lock);
}

void sp_connectivity_note_wifi_has_ip(bool has_ip)
{
    portENTER_CRITICAL(&s_lock);
    s_wifi_has_ip = has_ip;
    portEXIT_CRITICAL(&s_lock);
}

void sp_connectivity_set_mqtt_hooks(
    int64_t (*progress_age)(int64_t),
    bool (*progress_expired)(int64_t),
    int64_t (*next_attempt_in)(int64_t),
    int64_t (*last_verified_ms)(void)
)
{
    s_progress_age_fn = progress_age;
    s_progress_expired_fn = progress_expired;
    s_mqtt_next_attempt_fn = next_attempt_in;
    s_last_verified_fn = last_verified_ms;
}

void sp_connectivity_note_mqtt_verified(void)
{
    const int64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_mqtt_verified(&s_s3, now);
    sp_conn_s4_on_mqtt_verified(&s_s4, now);
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_stop_ok(void)
{
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_stop_ok(&s_s3, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_stop_fail(void)
{
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_stop_fail(&s_s3, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_start_ok(void)
{
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_start_ok(&s_s3, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_start_fail(void)
{
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_start_fail(&s_s3, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_connect_started(void)
{
    portENTER_CRITICAL(&s_lock);
    sp_conn_s3_on_connect_started(&s_s3, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static void s3_on_got_ip(uint32_t iface_generation)
{
    portENTER_CRITICAL(&s_lock);
    (void)sp_conn_s3_on_got_ip(&s_s3, iface_generation, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static const char *stage_str(sp_conn_stage_t stage)
{
    switch (stage) {
    case SP_CONN_STAGE_OK:
        return "ok";
    case SP_CONN_STAGE_NO_IP:
        return "no_ip";
    case SP_CONN_STAGE_MQTT_BACKOFF:
        return "mqtt_backoff";
    case SP_CONN_STAGE_MQTT_NO_PROGRESS:
        return "mqtt_no_progress";
    case SP_CONN_STAGE_WORKER_STALLED:
        return "worker_stalled";
    case SP_CONN_STAGE_DISPATCH_STALLED:
        return "dispatch_stalled";
    case SP_CONN_STAGE_TIMER_ARM_FAULT:
        return "timer_arm_fault";
    case SP_CONN_STAGE_INIT_FAILED:
        return "init_failed";
    default:
        return "unknown";
    }
}

static const char *s3_state_str(sp_conn_s3_state_t state)
{
    switch (state) {
    case SP_CONN_S3_IDLE:
        return "idle";
    case SP_CONN_S3_ACTIVE:
        return "active";
    case SP_CONN_S3_COOLDOWN:
        return "cooldown";
    case SP_CONN_S3_EXHAUSTED:
        return "exhausted";
    case SP_CONN_S3_BLOCKED_BY_WORKER:
        return "blocked_by_worker";
    default:
        return "unknown";
    }
}

static void apply_action(sp_conn_sm_result_t result)
{
    if (result.action == SP_CONN_ACTION_REQUEST_MQTT_RECOVERY) {
        portENTER_CRITICAL(&s_lock);
        if (s_sm.recovery_request_active) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        sp_conn_sm_note_recovery_started(&s_sm);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "supervisor requesting MQTT recovery (stage=%s)",
                 stage_str(result.stage));
        if (result.stage == SP_CONN_STAGE_MQTT_NO_PROGRESS) {
            sp_net_dispatch_post(SP_NET_OP_MQTT_PROGRESS);
        } else {
            sp_mqtt_request_reconnect();
            sp_net_dispatch_post(SP_NET_OP_MQTT_RECONNECT);
        }
    } else if (result.action == SP_CONN_ACTION_RETRY_TIMER_PATH) {
        portENTER_CRITICAL(&s_lock);
        sp_conn_sm_note_timer_retry(&s_sm);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "supervisor retrying MQTT timer path (fail=%d retries=%lu)",
                 (int)result.fail_reason,
                 (unsigned long)s_sm.timer_arm_retries);
        if (result.fail_reason == SP_CONN_FAIL_MQTT_PROGRESS_TIMER) {
            sp_net_dispatch_post_after(
                SP_NET_OP_MQTT_PROGRESS, SP_CONN_TIMER_RETRY_MIN_MS
            );
        } else {
            sp_net_dispatch_post_after(
                SP_NET_OP_MQTT_RECONNECT, SP_CONN_TIMER_RETRY_MIN_MS
            );
        }
    } else if (result.action == SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL) {
        ESP_LOGE(
            TAG,
            "supervisor detected unrecoverable stall (stage=%s)",
            stage_str(result.stage)
        );
    }
}

static void apply_stage3(sp_conn_s3_eval_t s3)
{
    if (s3.action == SP_CONN_S3_ACTION_BEGIN_IFACE_RESTART) {
        uint32_t gen;
        portENTER_CRITICAL(&s_lock);
        sp_conn_s3_note_begin(&s_s3, now_ms());
        gen = s_s3.iface_generation;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(
            TAG,
            "Stage 3 beginning Wi-Fi interface restart (gen=%lu attempts=%lu)",
            (unsigned long)gen,
            (unsigned long)s_s3.attempt_count
        );
        sp_wifi_request_iface_restart(gen);
    } else if (s3.action == SP_CONN_S3_ACTION_RECORD_BLOCKED) {
        ESP_LOGE(
            TAG,
            "Stage 3 escalation blocked by unresponsive sp_net "
            "(STAGE3_BLOCKED_BY_WORKER) — not attempting Wi-Fi stop/start"
        );
    }
}

static void execute_stage4_reboot(void)
{
    bool armed;
    portENTER_CRITICAL(&s_lock);
    armed = s_s4_reboot_armed;
    if (!armed) {
        s_s4_reboot_armed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (armed) {
        return;
    }

    ESP_LOGE(
        TAG,
        "Stage 4 controlled reboot: no verified MQTT progress for >= %lu ms",
        (unsigned long)SP_CONN_STAGE4_NO_PROGRESS_MS
    );

    /* Reuse OTA reboot-pending freeze (no new lifecycle mode). */
    sp_lifecycle_enter_ota_reboot_pending();
    sp_mqtt_prepare_ota_reboot();
    sp_wifi_prepare_ota_reboot();
    vTaskDelay(pdMS_TO_TICKS(SP_CONN_STAGE4_REBOOT_CLEANUP_MS));
    esp_restart();
}

static void apply_stage4(sp_conn_s4_eval_t s4)
{
    if (s4.action == SP_CONN_S4_ACTION_EXECUTE_REBOOT) {
        /* Direct reboot — no Observation END / OTA acoustic-close. */
        execute_stage4_reboot();
    }
}

static void fill_stage3_status(sp_conn_status_t *status, int64_t now)
{
    status->stage3_state = s_s3.state;
    status->stage3_phase = s_s3.phase;
    status->stage3_attempts = s_s3.attempt_count;
    status->stage3_consecutive_failures = s_s3.consecutive_failures;
    status->stage3_op_elapsed_ms =
        (s_s3.state == SP_CONN_S3_ACTIVE && s_s3.op_started_ms > 0)
            ? (now - s_s3.op_started_ms)
            : 0;
    status->stage3_last_result = s_s3.last_result;
    status->stage3_stop_completed = s_s3.stop_completed;
    status->stage3_start_completed = s_s3.start_completed;
    status->stage3_got_ip = s_s3.got_ip;
    status->stage3_mqtt_verified = s_s3.mqtt_verified;
    status->stage3_blocked_by_worker =
        (s_s3.state == SP_CONN_S3_BLOCKED_BY_WORKER);
    status->stage3_iface_generation = s_s3.iface_generation;
}

static void fill_stage4_status(sp_conn_status_t *status, int64_t age)
{
    status->stage4_state = s_s4.state;
    status->stage4_no_progress_age_ms = age;
    status->stage4_reboot_armed = s_s4_reboot_armed;
}

static const char *s4_state_str(sp_conn_s4_state_t state)
{
    switch (state) {
    case SP_CONN_S4_IDLE:
        return "idle";
    case SP_CONN_S4_REBOOT_PENDING:
        return "reboot_pending";
    default:
        return "unknown";
    }
}

static void supervisor_task(void *arg)
{
    sp_conn_stage_t logged = SP_CONN_STAGE_OK;
    sp_conn_s3_state_t logged_s3 = SP_CONN_S3_IDLE;
    sp_conn_s4_state_t logged_s4 = SP_CONN_S4_IDLE;
    (void)arg;
    for (;;) {
        const int64_t now = now_ms();
        sp_net_dispatch_monitor_t mon;
        sp_conn_sm_input_t in = {0};
        sp_conn_sm_result_t result;
        sp_conn_s3_input_t s3in = {0};
        sp_conn_s3_eval_t s3;
        sp_conn_s4_input_t s4in = {0};
        sp_conn_s4_eval_t s4;
        sp_conn_status_t status;
        bool worker_stalled;

        sp_net_dispatch_get_monitor(&mon, now);

        in.now_ms = now;
        portENTER_CRITICAL(&s_lock);
        in.timer_arm_failed = s_timer_arm_failed;
        in.timer_arm_retries = s_sm.timer_arm_retries;
        in.recovery_request_active = s_sm.recovery_request_active;
        (void)s_timer_fail;
        (void)s_wifi_has_ip;
        (void)s_mqtt_connected;
        portEXIT_CRITICAL(&s_lock);

        in.wifi_has_ip = sp_wifi_has_ip();
        in.mqtt_connected = sp_mqtt_is_connected();
        in.worker_started = mon.worker_started;
        in.supervisor_started = true;
        in.dispatch_pending = mon.pending;
        in.worker_heartbeat_age_ms = age_of(mon.heartbeat_ms, now);
        in.last_dispatch_age_ms = age_of(mon.last_dispatch_ms, now);
        in.pending_age_ms = age_of(mon.pending_since_ms, now);

        if (s_progress_expired_fn != NULL) {
            in.mqtt_progress_expired = s_progress_expired_fn(now);
        }
        if (s_progress_age_fn != NULL) {
            in.mqtt_progress_age_ms = s_progress_age_fn(now);
        } else {
            in.mqtt_progress_age_ms = -1;
        }
        if (s_mqtt_next_attempt_fn != NULL) {
            in.mqtt_next_attempt_in_ms = s_mqtt_next_attempt_fn(now);
            in.mqtt_backoff_waiting =
                !in.mqtt_connected && in.mqtt_next_attempt_in_ms > 0;
        }

        portENTER_CRITICAL(&s_lock);
        result = sp_conn_sm_evaluate(&s_sm, &in);
        worker_stalled =
            (result.stage == SP_CONN_STAGE_WORKER_STALLED
             || result.stage == SP_CONN_STAGE_DISPATCH_STALLED);

        s3in.now_ms = now;
        s3in.supervisor_up_ms = (s_started_ms > 0) ? (now - s_started_ms) : 0;
        s3in.last_verified_ms =
            (s_last_verified_fn != NULL) ? s_last_verified_fn() : 0;
        s3in.worker_responsive =
            mon.worker_started && !worker_stalled
            && (in.worker_heartbeat_age_ms < 0
                || (uint32_t)in.worker_heartbeat_age_ms < SP_CONN_WORKER_STALL_MS
                || !in.dispatch_pending);

        s3 = sp_conn_s3_evaluate(&s_s3, &s3in);

        s4in.now_ms = now;
        s4in.last_verified_ms = s3in.last_verified_ms;
        /* Read outside would race; lifecycle flag is a RAM bool (no mutex). */
        s4in.reboot_pending_other = sp_lifecycle_ota_reboot_pending();
        s4 = sp_conn_s4_evaluate(&s_s4, &s4in);

        status.wifi_has_ip = in.wifi_has_ip;
        status.mqtt_connected = in.mqtt_connected;
        status.mqtt_progress_age_ms = in.mqtt_progress_age_ms;
        status.stage = result.stage;
        status.stage_age_ms = now - s_sm.stage_since_ms;
        status.worker_heartbeat_age_ms = in.worker_heartbeat_age_ms;
        status.last_dispatch_age_ms = in.last_dispatch_age_ms;
        status.dispatch_pending = in.dispatch_pending;
        status.last_fail = s_sm.last_fail;
        status.supervisor_running = true;
        status.worker_started = in.worker_started;
        fill_stage3_status(&status, now);
        fill_stage4_status(&status, s4.no_progress_age_ms);
        s_last_status = status;
        portEXIT_CRITICAL(&s_lock);

        if (result.stage != logged) {
            ESP_LOGI(
                TAG,
                "stage %s -> %s (ip=%d mqtt=%d progress_age_ms=%lld "
                "hb_age_ms=%lld pending=%d)",
                stage_str(logged),
                stage_str(result.stage),
                in.wifi_has_ip ? 1 : 0,
                in.mqtt_connected ? 1 : 0,
                (long long)in.mqtt_progress_age_ms,
                (long long)in.worker_heartbeat_age_ms,
                in.dispatch_pending ? 1 : 0
            );
            logged = result.stage;
        }
        if (s3.state != logged_s3) {
            ESP_LOGI(
                TAG,
                "stage3 %s -> %s (result=%d gen=%lu attempts=%lu)",
                s3_state_str(logged_s3),
                s3_state_str(s3.state),
                (int)s3.last_result,
                (unsigned long)s3.iface_generation,
                (unsigned long)s_s3.attempt_count
            );
            logged_s3 = s3.state;
        }
        if (s4.state != logged_s4) {
            ESP_LOGI(
                TAG,
                "stage4 %s -> %s (no_progress_age_ms=%lld)",
                s4_state_str(logged_s4),
                s4_state_str(s4.state),
                (long long)s4.no_progress_age_ms
            );
            logged_s4 = s4.state;
        }

        apply_action(result);
        apply_stage3(s3);
        apply_stage4(s4);
        vTaskDelay(pdMS_TO_TICKS(SP_CONN_SUPERVISOR_PERIOD_MS));
    }
}

void sp_connectivity_start(void)
{
    if (s_task != NULL) {
        return;
    }
    sp_conn_sm_init(&s_sm);
    sp_conn_s3_sm_init(&s_s3);
    sp_conn_s4_sm_init(&s_s4);
    s_s4_reboot_armed = false;
    s_started_ms = now_ms();
    sp_wifi_set_stage3_hooks(
        s3_on_stop_ok,
        s3_on_stop_fail,
        s3_on_start_ok,
        s3_on_start_fail,
        s3_on_connect_started,
        s3_on_got_ip
    );
    if (xTaskCreate(supervisor_task, "sp_conn", 4096, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "failed to start connectivity supervisor");
        portENTER_CRITICAL(&s_lock);
        s_sm.stage = SP_CONN_STAGE_INIT_FAILED;
        s_sm.last_fail = SP_CONN_FAIL_SUPERVISOR_TASK;
        s_last_status.stage = SP_CONN_STAGE_INIT_FAILED;
        s_last_status.last_fail = SP_CONN_FAIL_SUPERVISOR_TASK;
        s_last_status.supervisor_running = false;
        fill_stage3_status(&s_last_status, now_ms());
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    ESP_LOGI(
        TAG,
        "connectivity supervisor started (period=%lu ms, stage3_escalate=%lu ms, "
        "stage4_no_progress=%lu ms)",
        (unsigned long)SP_CONN_SUPERVISOR_PERIOD_MS,
        (unsigned long)SP_CONN_STAGE3_ESCALATE_MS,
        (unsigned long)SP_CONN_STAGE4_NO_PROGRESS_MS
    );
}

bool sp_connectivity_is_running(void)
{
    return s_task != NULL;
}

void sp_connectivity_get_status(sp_conn_status_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_last_status;
    portEXIT_CRITICAL(&s_lock);
}
