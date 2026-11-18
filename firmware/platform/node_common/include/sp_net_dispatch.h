#pragma once

/**
 * Stage 1 recovery dispatch + Stage 2 monitor hooks.
 *
 * Timer and event callbacks may only call sp_net_dispatch_sm_request() /
 * sp_net_dispatch_post(). That records one bit and does not run Wi-Fi or
 * MQTT driver calls. The worker drains the bits and runs the bound operation.
 *
 * Heartbeat is updated when the worker is at the top of its loop or finishes
 * an op. The connectivity supervisor observes ages independently.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SP_NET_OP_WIFI_CONNECT = 0,
    SP_NET_OP_WIFI_DISCONNECT_FOR_RETRY,
    SP_NET_OP_WIFI_ARM_TIMER,
    /** Stage 3: esp_wifi_stop / start on a responsive worker. */
    SP_NET_OP_WIFI_IFACE_RESTART,
    SP_NET_OP_MQTT_RECONNECT,
    SP_NET_OP_MQTT_WATCHDOG,
    SP_NET_OP_MQTT_PROGRESS,
    SP_NET_OP_COUNT
} sp_net_op_t;

typedef struct {
    uint32_t pending;
    uint32_t request_count;
    uint32_t take_count;
} sp_net_dispatch_sm_t;

typedef void (*sp_net_dispatch_exec_fn)(sp_net_op_t op, void *ctx);

void sp_net_dispatch_sm_init(sp_net_dispatch_sm_t *sm);

/** Timer/event context. Sets a bit. Does not invoke exec. */
void sp_net_dispatch_sm_request(sp_net_dispatch_sm_t *sm, sp_net_op_t op);

/** Worker context. One pending op, lowest enum value first. */
bool sp_net_dispatch_sm_take(sp_net_dispatch_sm_t *sm, sp_net_op_t *out);

void sp_net_dispatch_sm_drain(
    sp_net_dispatch_sm_t *sm,
    sp_net_dispatch_exec_fn exec,
    void *ctx
);

typedef void (*sp_net_op_fn)(void);

typedef struct {
    bool worker_started;
    bool pending;
    int64_t heartbeat_ms;
    int64_t last_dispatch_ms;
    int64_t pending_since_ms;
} sp_net_dispatch_monitor_t;

/** Create the worker once. Safe to call from Wi-Fi and MQTT start. */
void sp_net_dispatch_start(void);

void sp_net_dispatch_bind(sp_net_op_t op, sp_net_op_fn fn);

/**
 * Non-blocking. Safe from an esp_timer callback.
 * delay_ms uses the caller's existing backoff; it is not a new timeout policy.
 */
void sp_net_dispatch_post(sp_net_op_t op);
void sp_net_dispatch_post_after(sp_net_op_t op, uint32_t delay_ms);

bool sp_net_dispatch_is_worker(void);
bool sp_net_dispatch_worker_started(void);

/** Snapshot for the connectivity supervisor (monotonic ms timestamps). */
void sp_net_dispatch_get_monitor(sp_net_dispatch_monitor_t *out, int64_t now_ms);

#ifdef __cplusplus
}
#endif
