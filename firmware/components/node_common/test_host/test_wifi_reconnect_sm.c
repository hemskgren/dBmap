/**
 * Host unit tests for dbmap_wifi_reconnect_sm.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/components/node_common/include \
 *     firmware/components/node_common/wifi_reconnect_sm.c \
 *     firmware/components/node_common/test_host/test_wifi_reconnect_sm.c \
 *     -o /tmp/test_dbmap_wifi_reconnect_sm && /tmp/test_dbmap_wifi_reconnect_sm
 */
#include "dbmap_wifi_reconnect_sm.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_connect_accepted_then_got_ip(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_sm_result_t start =
        dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    EXPECT(start.action == DBMAP_WIFI_SM_START_CONNECT);
    EXPECT(sm.in_flight == true);

    dbmap_wifi_sm_result_t accepted =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    EXPECT(accepted.arm_watchdog == true);
    EXPECT(accepted.watchdog_generation == 1);
    EXPECT(sm.watchdog_armed == true);

    dbmap_wifi_sm_result_t got_ip = dbmap_wifi_reconnect_sm_on_got_ip(&sm);
    EXPECT(got_ip.ip_acquired == true);
    EXPECT(got_ip.cancel_reconnect_timer == true);
    EXPECT(got_ip.cancel_watchdog == true);
    EXPECT(sm.in_flight == false);
    EXPECT(sm.watchdog_armed == false);
    EXPECT(sm.disconnect_counted == false);
}

static void test_connect_accepted_then_disconnected(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);

    dbmap_wifi_sm_result_t disc =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(sm.in_flight == false);
    EXPECT(disc.cancel_watchdog == true);
    EXPECT(disc.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(dbmap_wifi_reconnect_sm_take_disconnect_diag(&sm) == true);
    EXPECT(dbmap_wifi_reconnect_sm_take_disconnect_diag(&sm) == false);
}

static void test_connect_accepted_no_event_timeout_recovery(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    dbmap_wifi_sm_result_t accepted =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    uint32_t gen = accepted.watchdog_generation;

    dbmap_wifi_sm_result_t wd =
        dbmap_wifi_reconnect_sm_on_watchdog(&sm, gen, false);
    EXPECT(wd.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(sm.in_flight == false);
    EXPECT(sm.watchdog_armed == false);
}

static void test_stale_watchdog_after_newer_attempt(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    dbmap_wifi_sm_result_t first =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    uint32_t stale_gen = first.watchdog_generation;

    (void)dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(sm.reconnect_armed == true);
    dbmap_wifi_sm_result_t timer =
        dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    EXPECT(timer.action == DBMAP_WIFI_SM_START_CONNECT);
    dbmap_wifi_sm_result_t second =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    EXPECT(second.watchdog_generation != stale_gen);

    dbmap_wifi_sm_result_t stale =
        dbmap_wifi_reconnect_sm_on_watchdog(&sm, stale_gen, false);
    EXPECT(stale.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.in_flight == true);
    EXPECT(sm.watchdog_armed == true);
}

static void test_two_concurrent_scheduling_requests(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_sm_result_t first =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(first.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);

    /* Second request while reservation held — must not schedule again. */
    dbmap_wifi_sm_result_t second =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(second.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(dbmap_wifi_reconnect_sm_can_schedule(&sm) == false);
}

static void test_exactly_one_reconnect_timer_reservation(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    uint32_t gen = sm.watchdog_generation;

    dbmap_wifi_sm_result_t wd =
        dbmap_wifi_reconnect_sm_on_watchdog(&sm, gen, false);
    EXPECT(wd.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);

    dbmap_wifi_sm_result_t disc =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(disc.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.reconnect_armed == true);

    dbmap_wifi_reconnect_sm_release_reconnect_reservation(&sm);
    EXPECT(sm.reconnect_armed == false);
    EXPECT(dbmap_wifi_reconnect_sm_can_schedule(&sm) == true);
}

static void test_disconnect_concurrent_with_watchdog_timeout(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    dbmap_wifi_sm_result_t accepted =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    uint32_t gen = accepted.watchdog_generation;

    /* Disconnect wins the reservation first. */
    dbmap_wifi_sm_result_t disc =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(disc.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(sm.watchdog_armed == false);

    /* Watchdog callback for same generation arrives late — no second schedule. */
    dbmap_wifi_sm_result_t wd =
        dbmap_wifi_reconnect_sm_on_watchdog(&sm, gen, false);
    EXPECT(wd.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.reconnect_armed == true);

    /* Inverse order: watchdog first, then disconnect. */
    dbmap_wifi_reconnect_sm_init(&sm, 30000);
    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    accepted = dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    gen = accepted.watchdog_generation;

    wd = dbmap_wifi_reconnect_sm_on_watchdog(&sm, gen, false);
    EXPECT(wd.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);

    disc = dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(disc.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.reconnect_armed == true);
}

static void test_boot_timeout_then_late_got_ip(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    EXPECT(sm.connect_generation == 1);

    dbmap_wifi_sm_result_t boot =
        dbmap_wifi_reconnect_sm_on_boot_wait_timeout(&sm);
    EXPECT(boot.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(boot.cancel_watchdog == true);
    EXPECT(sm.in_flight == false);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(sm.allow_late_ip == true);
    EXPECT(sm.late_ip_generation == 1);

    /* Late GOT_IP for abandoned boot attempt — accept and drop reservation. */
    dbmap_wifi_sm_result_t late = dbmap_wifi_reconnect_sm_on_got_ip(&sm);
    EXPECT(late.ip_acquired == true);
    EXPECT(late.cancel_reconnect_timer == true);
    EXPECT(sm.reconnect_armed == false);
    EXPECT(sm.allow_late_ip == false);
}

static void test_boot_timeout_late_got_ip_ignored_after_newer_attempt(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);

    (void)dbmap_wifi_reconnect_sm_on_boot_wait_timeout(&sm);
    EXPECT(sm.reconnect_armed == true);

    /* Newer attempt starts before late IP arrives. */
    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    EXPECT(sm.connect_generation == 2);
    EXPECT(sm.in_flight == true);
    EXPECT(sm.allow_late_ip == false);

    /* Stale GOT_IP from gen 1 must not clear the newer attempt. */
    /* Simulate by forcing allow_late_ip for gen 1 while gen 2 is active —
     * on_connect_accepted already cleared allow_late_ip; a pure stale event: */
    dbmap_wifi_sm_result_t stale = dbmap_wifi_reconnect_sm_on_got_ip(&sm);
    /* Gen 2 is in_flight, so GOT_IP is accepted for the *current* attempt. */
    EXPECT(stale.ip_acquired == true);
    EXPECT(sm.in_flight == false);

    /* Explicit stale after newer attempt abandoned mid-flight: */
    dbmap_wifi_reconnect_sm_init(&sm, 30000);
    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm); /* gen 1 */
    (void)dbmap_wifi_reconnect_sm_on_boot_wait_timeout(&sm);
    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    (void)dbmap_wifi_reconnect_sm_on_connect_accepted(&sm); /* gen 2 */
    (void)dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(sm.reconnect_armed == true);
    EXPECT(sm.connect_generation == 2);
    EXPECT(sm.in_flight == false);
    /* Late IP from abandoned gen 1: allow_late_ip false, not in_flight. */
    stale = dbmap_wifi_reconnect_sm_on_got_ip(&sm);
    EXPECT(stale.ip_acquired == false);
    EXPECT(sm.reconnect_armed == true);
}

static void test_ota_prepare_while_watchdog_armed(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    dbmap_wifi_reconnect_sm_claim_in_flight(&sm);
    dbmap_wifi_sm_result_t accepted =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);
    EXPECT(sm.watchdog_armed == true);
    EXPECT(accepted.arm_watchdog == true);

    dbmap_wifi_sm_result_t prep =
        dbmap_wifi_reconnect_sm_prepare_ota_reboot(&sm);
    EXPECT(prep.cancel_reconnect_timer == true);
    EXPECT(prep.cancel_watchdog == true);
    EXPECT(sm.in_flight == false);
    EXPECT(sm.reconnect_armed == false);
    EXPECT(sm.watchdog_armed == false);
    EXPECT(sm.allow_late_ip == false);

    /* Stale watchdog after OTA prepare must not schedule. */
    dbmap_wifi_sm_result_t wd = dbmap_wifi_reconnect_sm_on_watchdog(
        &sm, accepted.watchdog_generation, false
    );
    EXPECT(wd.action == DBMAP_WIFI_SM_NONE);
}

static void test_ota_pending_behavior(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);

    (void)dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, false, false);
    dbmap_wifi_sm_result_t accepted =
        dbmap_wifi_reconnect_sm_on_connect_accepted(&sm);

    dbmap_wifi_sm_result_t disc =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, true);
    EXPECT(disc.action == DBMAP_WIFI_SM_NONE);
    EXPECT(disc.cancel_watchdog == true);

    dbmap_wifi_sm_result_t wd = dbmap_wifi_reconnect_sm_on_watchdog(
        &sm, accepted.watchdog_generation, true
    );
    EXPECT(wd.action == DBMAP_WIFI_SM_NONE);
}

static void test_reconnect_timer_skips_when_has_ip(void)
{
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);
    sm.reconnect_armed = true;
    dbmap_wifi_sm_result_t r =
        dbmap_wifi_reconnect_sm_on_reconnect_timer(&sm, true, false);
    EXPECT(r.action == DBMAP_WIFI_SM_NONE);
    EXPECT(sm.reconnect_armed == false);
    EXPECT(sm.in_flight == false);
}

static void test_in_flight_timeout_constant_matches_boot_wait(void)
{
    EXPECT(DBMAP_WIFI_IN_FLIGHT_TIMEOUT_MS == 30000u);
}

static void test_reason_2_and_205_generic_path(void)
{
    /* Reasons 2/205 share on_disconnected — no special branch in the SM. */
    dbmap_wifi_reconnect_sm_t sm;
    dbmap_wifi_reconnect_sm_init(&sm, 30000);
    dbmap_wifi_sm_result_t r =
        dbmap_wifi_reconnect_sm_on_disconnected(&sm, false);
    EXPECT(r.action == DBMAP_WIFI_SM_SCHEDULE_RECONNECT);
    EXPECT(sm.reconnect_armed == true);
}

int main(void)
{
    test_connect_accepted_then_got_ip();
    test_connect_accepted_then_disconnected();
    test_connect_accepted_no_event_timeout_recovery();
    test_stale_watchdog_after_newer_attempt();
    test_two_concurrent_scheduling_requests();
    test_exactly_one_reconnect_timer_reservation();
    test_disconnect_concurrent_with_watchdog_timeout();
    test_boot_timeout_then_late_got_ip();
    test_boot_timeout_late_got_ip_ignored_after_newer_attempt();
    test_ota_prepare_while_watchdog_armed();
    test_ota_pending_behavior();
    test_reconnect_timer_skips_when_has_ip();
    test_in_flight_timeout_constant_matches_boot_wait();
    test_reason_2_and_205_generic_path();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
