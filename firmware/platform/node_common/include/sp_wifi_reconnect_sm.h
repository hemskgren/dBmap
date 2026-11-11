#pragma once

/**
 * Host-testable Wi-Fi reconnect / in-flight watchdog state machine.
 *
 * Pure logic — no ESP-IDF. net_wifi.c owns timers, driver calls, and logging.
 *
 * Ownership:
 * - ``reconnect_armed`` is reserved atomically inside SM transitions that return
 *   SCHEDULE_RECONNECT. The glue layer then creates exactly one oneshot timer for
 *   that reservation. On timer create/start failure the glue must call
 *   ``release_reconnect_reservation``. Timer fire / GOT_IP / OTA clear the flag
 *   via SM; the glue only deletes the esp_timer handle.
 * - ``in_flight`` / ``watchdog_*`` / ``connect_generation`` are SM-owned.
 * - Callers that mutate the SM from concurrent tasks must serialize SM entry
 *   (short critical section), then apply timer/driver/log side effects outside it.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bound for "esp_wifi_connect() accepted but neither GOT_IP nor DISCONNECTED".
 * Matches sp_wifi_start()'s 30 s GOT_IP wait (association + DHCP).
 */
#define SP_WIFI_IN_FLIGHT_TIMEOUT_MS 30000u

typedef struct {
    bool reconnect_armed;
    bool in_flight;
    bool disconnect_counted;
    uint32_t connect_generation;
    uint32_t watchdog_generation;
    bool watchdog_armed;
    uint32_t in_flight_timeout_ms;
    /** When set, a late GOT_IP may still complete this abandoned generation. */
    bool allow_late_ip;
    uint32_t late_ip_generation;
} sp_wifi_reconnect_sm_t;

typedef enum {
    SP_WIFI_SM_NONE = 0,
    /**
     * Reconnect reservation is already held (``reconnect_armed``). Caller must
     * create the backoff oneshot or release the reservation on failure.
     */
    SP_WIFI_SM_SCHEDULE_RECONNECT,
    /** Caller should call esp_wifi_connect() now. */
    SP_WIFI_SM_START_CONNECT,
} sp_wifi_sm_action_t;

typedef struct {
    sp_wifi_sm_action_t action;
    /** Cancel reconnect oneshot if true (SM already cleared reconnect_armed). */
    bool cancel_reconnect_timer;
    /** Cancel in-flight watchdog if true. */
    bool cancel_watchdog;
    /** Arm watchdog for connect_generation after START_CONNECT succeeds. */
    bool arm_watchdog;
    uint32_t watchdog_generation;
    /** GOT_IP accepted for the current or abandoned-late generation. */
    bool ip_acquired;
} sp_wifi_sm_result_t;

void sp_wifi_reconnect_sm_init(
    sp_wifi_reconnect_sm_t *sm,
    uint32_t in_flight_timeout_ms
);

/** Whether a new reconnect reservation may be taken. */
bool sp_wifi_reconnect_sm_can_schedule(const sp_wifi_reconnect_sm_t *sm);

/**
 * Drop a reconnect reservation when timer create/start fails.
 * Not needed on the normal cancel path (SM clears armed itself).
 */
void sp_wifi_reconnect_sm_release_reconnect_reservation(
    sp_wifi_reconnect_sm_t *sm
);

/**
 * Claim in_flight before esp_wifi_connect() (boot path and timer path).
 * Blocks concurrent schedule reservations.
 */
void sp_wifi_reconnect_sm_claim_in_flight(sp_wifi_reconnect_sm_t *sm);

/**
 * STA_DISCONNECTED. Clears in_flight/watchdog. Atomically reserves reconnect
 * when allowed (non-OTA).
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_disconnected(
    sp_wifi_reconnect_sm_t *sm,
    bool ota_pending
);

/** Reconnect oneshot fired. Clears reservation; may claim in_flight. */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_reconnect_timer(
    sp_wifi_reconnect_sm_t *sm,
    bool has_ip,
    bool ota_pending
);

/**
 * After esp_wifi_connect() returns ESP_OK. Bumps generation, arms watchdog
 * request, and invalidates any late-IP allowance from a prior attempt.
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_connect_accepted(
    sp_wifi_reconnect_sm_t *sm
);

/**
 * After esp_wifi_connect() returns an error. Clears in_flight and atomically
 * reserves another reconnect when allowed.
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_connect_start_failed(
    sp_wifi_reconnect_sm_t *sm
);

/**
 * GOT_IP. Accepts current in-flight attempt, or a late IP for an abandoned
 * boot-timeout generation. Ignores stale IP once a newer attempt has started.
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_got_ip(
    sp_wifi_reconnect_sm_t *sm
);

/**
 * In-flight watchdog fired for ``generation``. Ignores stale generations.
 * On match: clears in_flight and atomically reserves SCHEDULE_RECONNECT.
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_watchdog(
    sp_wifi_reconnect_sm_t *sm,
    uint32_t generation,
    bool ota_pending
);

/**
 * Boot GOT_IP wait timed out. Clears in_flight/watchdog, allows late IP for
 * this generation, and atomically reserves reconnect — same semantics as the
 * in-flight watchdog recovery path.
 */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_boot_wait_timeout(
    sp_wifi_reconnect_sm_t *sm
);

/** OTA reboot freeze — drop pending reconnect intent flags. */
sp_wifi_sm_result_t sp_wifi_reconnect_sm_prepare_ota_reboot(
    sp_wifi_reconnect_sm_t *sm
);

/** First disconnect in a down-period should bump diag (caller tracks). */
bool sp_wifi_reconnect_sm_take_disconnect_diag(
    sp_wifi_reconnect_sm_t *sm
);

#ifdef __cplusplus
}
#endif
