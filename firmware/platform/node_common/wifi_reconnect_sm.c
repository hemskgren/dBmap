#include "sp_wifi_reconnect_sm.h"

#include <stddef.h>

static sp_wifi_sm_result_t result_none(void)
{
    sp_wifi_sm_result_t r = {
        .action = SP_WIFI_SM_NONE,
        .cancel_reconnect_timer = false,
        .cancel_watchdog = false,
        .arm_watchdog = false,
        .watchdog_generation = 0,
        .ip_acquired = false,
        .defer_arm_to_worker = false,
        .skip_backoff = false,
    };
    return r;
}

/**
 * Decision + reservation are one step: at most one SCHEDULE winner.
 * Caller creates the timer for the reservation outside any lock.
 */
static bool try_reserve_reconnect(sp_wifi_reconnect_sm_t *sm)
{
    if (sm->reconnect_armed || sm->in_flight) {
        return false;
    }
    sm->reconnect_armed = true;
    return true;
}

void sp_wifi_reconnect_sm_init(
    sp_wifi_reconnect_sm_t *sm,
    uint32_t in_flight_timeout_ms
)
{
    if (sm == NULL) {
        return;
    }
    sm->reconnect_armed = false;
    sm->in_flight = false;
    sm->disconnect_counted = false;
    sm->connect_generation = 0;
    sm->watchdog_generation = 0;
    sm->watchdog_armed = false;
    sm->in_flight_timeout_ms =
        in_flight_timeout_ms == 0 ? SP_WIFI_IN_FLIGHT_TIMEOUT_MS : in_flight_timeout_ms;
    sm->allow_late_ip = false;
    sm->late_ip_generation = 0;
    sm->disconnect_before_next_connect = false;
    sm->awaiting_disconnect = false;
    sm->accept_renewed_ip = false;
}

bool sp_wifi_reconnect_sm_can_schedule(const sp_wifi_reconnect_sm_t *sm)
{
    if (sm == NULL) {
        return false;
    }
    return !sm->reconnect_armed && !sm->in_flight;
}

void sp_wifi_reconnect_sm_release_reconnect_reservation(
    sp_wifi_reconnect_sm_t *sm
)
{
    if (sm != NULL) {
        sm->reconnect_armed = false;
    }
}

void sp_wifi_reconnect_sm_claim_in_flight(sp_wifi_reconnect_sm_t *sm)
{
    if (sm != NULL) {
        sm->in_flight = true;
    }
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_disconnected(
    sp_wifi_reconnect_sm_t *sm,
    bool ota_pending
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    const bool intentional = sm->awaiting_disconnect;
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->allow_late_ip = false;
    sm->accept_renewed_ip = false;
    sm->disconnect_before_next_connect = false;
    sm->awaiting_disconnect = false;
    r.cancel_watchdog = true;

    if (ota_pending) {
        return r;
    }
    if (intentional) {
        sm->in_flight = true;
        r.action = SP_WIFI_SM_START_CONNECT;
        r.skip_backoff = true;
        return r;
    }

    if (try_reserve_reconnect(sm)) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_reconnect_timer(
    sp_wifi_reconnect_sm_t *sm,
    bool has_ip,
    bool ota_pending
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    /* Timer consumed the reservation. */
    sm->reconnect_armed = false;
    if (ota_pending || has_ip || sm->in_flight) {
        if (ota_pending || has_ip) {
            sm->disconnect_before_next_connect = false;
            sm->awaiting_disconnect = false;
        }
        return r;
    }
    if (sm->disconnect_before_next_connect) {
        sm->awaiting_disconnect = true;
        sm->in_flight = true;
        if (sm->watchdog_generation < UINT32_MAX) {
            sm->watchdog_generation++;
        } else {
            sm->watchdog_generation = 1;
        }
        sm->watchdog_armed = true;
        r.cancel_watchdog = true;
        r.arm_watchdog = true;
        r.watchdog_generation = sm->watchdog_generation;
        r.action = SP_WIFI_SM_DISCONNECT_FOR_RETRY;
        return r;
    }
    sm->in_flight = true;
    r.action = SP_WIFI_SM_START_CONNECT;
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_connect_accepted(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    if (sm->connect_generation < UINT32_MAX) {
        sm->connect_generation++;
    } else {
        sm->connect_generation = 1;
    }
    sm->in_flight = true;
    sm->allow_late_ip = false;
    sm->accept_renewed_ip = false;
    sm->disconnect_before_next_connect = false;
    sm->awaiting_disconnect = false;
    sm->watchdog_generation = sm->connect_generation;
    sm->watchdog_armed = true;
    r.cancel_watchdog = true; /* replace any prior oneshot */
    r.arm_watchdog = true;
    r.watchdog_generation = sm->watchdog_generation;
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_connect_start_failed(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->allow_late_ip = false;
    sm->awaiting_disconnect = false;
    r.cancel_watchdog = true;
    sm->disconnect_before_next_connect = true;
    if (try_reserve_reconnect(sm)) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_got_ip(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }

    const bool current_attempt = sm->in_flight;
    const bool late_for_abandoned =
        !sm->in_flight
        && sm->allow_late_ip
        && sm->late_ip_generation == sm->connect_generation;
    const bool renewed = sm->accept_renewed_ip;

    if (!current_attempt && !late_for_abandoned && !renewed) {
        /* Stale GOT_IP after a newer attempt started (or unrelated). */
        return r;
    }

    sm->in_flight = false;
    sm->disconnect_counted = false;
    sm->watchdog_armed = false;
    sm->allow_late_ip = false;
    sm->accept_renewed_ip = false;
    sm->disconnect_before_next_connect = false;
    sm->awaiting_disconnect = false;
    sm->reconnect_armed = false;
    r.cancel_reconnect_timer = true;
    r.cancel_watchdog = true;
    r.ip_acquired = true;
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_watchdog(
    sp_wifi_reconnect_sm_t *sm,
    uint32_t generation,
    bool ota_pending
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    if (!sm->watchdog_armed || generation != sm->watchdog_generation) {
        return r;
    }
    sm->watchdog_armed = false;
    if (sm->awaiting_disconnect) {
        /* Disconnect event never arrived. Use the same 30 s watchdog. */
        sm->awaiting_disconnect = false;
        sm->disconnect_before_next_connect = false;
        sm->in_flight = true;
        r.action = SP_WIFI_SM_START_CONNECT;
        r.skip_backoff = true;
        return r;
    }
    if (sm->in_flight || sm->connect_generation != 0) {
        sm->allow_late_ip = true;
        sm->late_ip_generation = sm->connect_generation;
    }
    if (ota_pending) {
        sm->in_flight = false;
        sm->disconnect_before_next_connect = false;
        return r;
    }
    if (!sm->in_flight) {
        return r;
    }
    sm->in_flight = false;
    sm->disconnect_before_next_connect = true;
    if (try_reserve_reconnect(sm)) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_boot_wait_timeout(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    /* Allow a late GOT_IP for this generation until a newer connect starts. */
    if (sm->in_flight || sm->connect_generation != 0) {
        sm->allow_late_ip = true;
        sm->late_ip_generation = sm->connect_generation;
    }
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->disconnect_before_next_connect = true;
    r.cancel_watchdog = true;
    if (try_reserve_reconnect(sm)) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_prepare_ota_reboot(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    sm->reconnect_armed = false;
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->allow_late_ip = false;
    sm->accept_renewed_ip = false;
    sm->disconnect_before_next_connect = false;
    sm->awaiting_disconnect = false;
    r.cancel_reconnect_timer = true;
    r.cancel_watchdog = true;
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_lost_ip(
    sp_wifi_reconnect_sm_t *sm,
    bool ota_pending
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    sm->accept_renewed_ip = true;
    sm->allow_late_ip = false;
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->awaiting_disconnect = false;
    r.cancel_watchdog = true;
    if (ota_pending) {
        sm->disconnect_before_next_connect = false;
        return r;
    }
    sm->disconnect_before_next_connect = true;
    if (try_reserve_reconnect(sm)) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_reconnect_arm_failed(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    sm->in_flight = false;
    sm->awaiting_disconnect = false;
    sm->watchdog_armed = false;
    r.cancel_watchdog = true;
    if (!sm->reconnect_armed) {
        (void)try_reserve_reconnect(sm);
    }
    if (sm->reconnect_armed) {
        r.action = SP_WIFI_SM_SCHEDULE_RECONNECT;
        r.defer_arm_to_worker = true;
    }
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_disconnect_skipped(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    if (!sm->awaiting_disconnect && !sm->disconnect_before_next_connect) {
        return r;
    }
    sm->awaiting_disconnect = false;
    sm->disconnect_before_next_connect = false;
    sm->in_flight = false;
    sm->watchdog_armed = false;
    sm->reconnect_armed = false;
    r.cancel_watchdog = true;
    r.cancel_reconnect_timer = true;
    return r;
}

sp_wifi_sm_result_t sp_wifi_reconnect_sm_on_disconnect_api_failed(
    sp_wifi_reconnect_sm_t *sm
)
{
    sp_wifi_sm_result_t r = result_none();
    if (sm == NULL) {
        return r;
    }
    sm->awaiting_disconnect = false;
    sm->disconnect_before_next_connect = false;
    sm->watchdog_armed = false;
    sm->in_flight = true;
    r.cancel_watchdog = true;
    r.skip_backoff = true;
    return r;
}

bool sp_wifi_reconnect_sm_take_disconnect_diag(sp_wifi_reconnect_sm_t *sm)
{
    if (sm == NULL || sm->disconnect_counted) {
        return false;
    }
    sm->disconnect_counted = true;
    return true;
}
