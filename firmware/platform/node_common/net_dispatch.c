#include "sp_net_dispatch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sp_net";

static sp_net_dispatch_sm_t s_sm;
static sp_net_op_fn s_fns[SP_NET_OP_COUNT];
static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_delay_valid;
static int64_t s_delay_at_us;
static sp_net_op_t s_delay_op;
static int64_t s_heartbeat_ms;
static int64_t s_last_dispatch_ms;
static int64_t s_pending_since_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void stamp_pending_locked(void)
{
    if (s_sm.pending != 0 && s_pending_since_ms == 0) {
        s_pending_since_ms = now_ms();
    }
    if (s_sm.pending == 0) {
        s_pending_since_ms = 0;
    }
}

static void dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t wait_ticks = portMAX_DELAY;
        bool fire_delay = false;
        sp_net_op_t delayed = SP_NET_OP_WIFI_CONNECT;
        int64_t now_us = esp_timer_get_time();

        s_heartbeat_ms = now_ms();

        portENTER_CRITICAL(&s_lock);
        if (s_delay_valid) {
            if (now_us >= s_delay_at_us) {
                fire_delay = true;
                delayed = s_delay_op;
                s_delay_valid = false;
            } else {
                int64_t remain_us = s_delay_at_us - now_us;
                int64_t remain_ms = (remain_us + 999) / 1000;
                if (remain_ms < 1) {
                    remain_ms = 1;
                }
                wait_ticks = pdMS_TO_TICKS((uint32_t)remain_ms);
                if (wait_ticks < 1) {
                    wait_ticks = 1;
                }
            }
        }
        if (fire_delay) {
            sp_net_dispatch_sm_request(&s_sm, delayed);
            stamp_pending_locked();
        }
        portEXIT_CRITICAL(&s_lock);

        for (;;) {
            sp_net_op_t op;
            bool got;
            portENTER_CRITICAL(&s_lock);
            got = sp_net_dispatch_sm_take(&s_sm, &op);
            stamp_pending_locked();
            portEXIT_CRITICAL(&s_lock);
            if (!got) {
                break;
            }
            if (s_fns[op] != NULL) {
                s_fns[op]();
            } else {
                ESP_LOGW(TAG, "recovery op %d has no handler", (int)op);
            }
            s_last_dispatch_ms = now_ms();
            s_heartbeat_ms = s_last_dispatch_ms;
        }

        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

void sp_net_dispatch_start(void)
{
    if (s_task != NULL) {
        return;
    }
    sp_net_dispatch_sm_init(&s_sm);
    s_heartbeat_ms = 0;
    s_last_dispatch_ms = 0;
    s_pending_since_ms = 0;
    /* Below mic (5) and observation (4) so recovery does not preempt capture. */
    if (xTaskCreate(dispatch_task, "sp_net", 8192, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "failed to start network recovery task");
    } else {
        s_heartbeat_ms = now_ms();
    }
}

void sp_net_dispatch_bind(sp_net_op_t op, sp_net_op_fn fn)
{
    if (op >= SP_NET_OP_COUNT) {
        return;
    }
    s_fns[op] = fn;
}

void sp_net_dispatch_post(sp_net_op_t op)
{
    if (s_task == NULL || op >= SP_NET_OP_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    sp_net_dispatch_sm_request(&s_sm, op);
    stamp_pending_locked();
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
}

void sp_net_dispatch_post_after(sp_net_op_t op, uint32_t delay_ms)
{
    int64_t at;
    if (s_task == NULL || op >= SP_NET_OP_COUNT) {
        return;
    }
    if (delay_ms < 1) {
        delay_ms = 1;
    }
    at = esp_timer_get_time() + ((int64_t)delay_ms * 1000);
    portENTER_CRITICAL(&s_lock);
    s_delay_op = op;
    s_delay_at_us = at;
    s_delay_valid = true;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
}

bool sp_net_dispatch_is_worker(void)
{
    return s_task != NULL && xTaskGetCurrentTaskHandle() == s_task;
}

bool sp_net_dispatch_worker_started(void)
{
    return s_task != NULL;
}

void sp_net_dispatch_get_monitor(sp_net_dispatch_monitor_t *out, int64_t now)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    out->worker_started = (s_task != NULL);
    out->pending = (s_sm.pending != 0) || s_delay_valid;
    out->heartbeat_ms = s_heartbeat_ms;
    out->last_dispatch_ms = s_last_dispatch_ms;
    out->pending_since_ms = s_pending_since_ms;
    if (s_delay_valid && s_pending_since_ms == 0) {
        /* Delayed work counts as pending for stall detection once due; until
         * then pending_since stays 0 so backoff waits are not stalls. */
        out->pending = out->pending;
    }
    (void)now;
    portEXIT_CRITICAL(&s_lock);
}
