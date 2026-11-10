/**
 * Host unit tests for Ear Design B boot ordering policy.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/esp-ear/main \
 *     firmware/esp-ear/main/ear_boot_order.c \
 *     firmware/esp-ear/main/test_host/test_ear_boot_order.c \
 *     -o /tmp/test_ear_boot_order && /tmp/test_ear_boot_order
 */

#include <stdio.h>
#include <stdlib.h>

#include "ear_boot_order.h"

static int g_failures;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(                                                           \
                stderr,                                                        \
                "FAIL %s:%d: %s\n",                                            \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #cond                                                          \
            );                                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_mic_after_ipv4_once(void)
{
    dbmap_ear_boot_state_t s = {0};

    EXPECT(!dbmap_ear_boot_allow_mic_start(&s));

    s.has_ipv4 = true;
    EXPECT(dbmap_ear_boot_allow_mic_start(&s));

    s.mic_start_attempted = true;
    EXPECT(!dbmap_ear_boot_allow_mic_start(&s));
}

static void test_obs_only_after_desired_once(void)
{
    dbmap_ear_boot_state_t s = {
        .has_ipv4 = true,
        .mic_start_attempted = true,
        .desired_boot_done = false,
        .obs_start_attempted = false,
    };
    EXPECT(!dbmap_ear_boot_allow_obs_start(&s));

    s.desired_boot_done = true;
    EXPECT(dbmap_ear_boot_allow_obs_start(&s));

    s.obs_start_attempted = true;
    EXPECT(!dbmap_ear_boot_allow_obs_start(&s));
}

static void test_design_b_sequence_gates(void)
{
    /* After IPv4, before Desired: mic yes, obs no. */
    dbmap_ear_boot_state_t pre_mqtt = {
        .has_ipv4 = true,
        .mic_start_attempted = false,
        .desired_boot_done = false,
        .obs_start_attempted = false,
    };
    EXPECT(dbmap_ear_boot_allow_mic_start(&pre_mqtt));
    EXPECT(!dbmap_ear_boot_allow_obs_start(&pre_mqtt));

    /* During MQTT/OTA/Desired with mic already attempted: neither restarts. */
    dbmap_ear_boot_state_t during_desired = {
        .has_ipv4 = true,
        .mic_start_attempted = true,
        .desired_boot_done = false,
        .obs_start_attempted = false,
    };
    EXPECT(!dbmap_ear_boot_allow_mic_start(&during_desired));
    EXPECT(!dbmap_ear_boot_allow_obs_start(&during_desired));

    /* After Desired: obs yes; mic still blocked by one-shot. */
    dbmap_ear_boot_state_t post_desired = {
        .has_ipv4 = true,
        .mic_start_attempted = true,
        .desired_boot_done = true,
        .obs_start_attempted = false,
    };
    EXPECT(!dbmap_ear_boot_allow_mic_start(&post_desired));
    EXPECT(dbmap_ear_boot_allow_obs_start(&post_desired));

    /* Late IPv4 catch-up: mic allowed before obs when Desired is done. */
    dbmap_ear_boot_state_t late_ip = {
        .has_ipv4 = true,
        .mic_start_attempted = false,
        .desired_boot_done = true,
        .obs_start_attempted = false,
    };
    EXPECT(dbmap_ear_boot_allow_mic_start(&late_ip));
    EXPECT(dbmap_ear_boot_allow_obs_start(&late_ip));
}

static void test_null_state_rejected(void)
{
    EXPECT(!dbmap_ear_boot_allow_mic_start(NULL));
    EXPECT(!dbmap_ear_boot_allow_obs_start(NULL));
}

int main(void)
{
    test_mic_after_ipv4_once();
    test_obs_only_after_desired_once();
    test_design_b_sequence_gates();
    test_null_state_rejected();

    if (g_failures != 0) {
        fprintf(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    puts("test_ear_boot_order: ok");
    return 0;
}
