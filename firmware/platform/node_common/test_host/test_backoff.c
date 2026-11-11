/**
 * Host unit tests for sp_backoff (no ESP-IDF required).
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/components/node_common/include \
 *     -I firmware/components/node_common/test_host \
 *     firmware/components/node_common/backoff.c \
 *     firmware/components/node_common/test_host/test_backoff.c \
 *     -o /tmp/test_sp_backoff && /tmp/test_sp_backoff
 */
#include "sp_backoff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static uint32_t fixed_rand(void)
{
    return 0;
}

static uint32_t mid_rand(void)
{
    return 0x7fffffffu;
}

static void test_growth_and_cap(void)
{
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 0) == 1000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 1) == 1000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 2) == 2000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 3) == 4000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 4) == 8000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 5) == 16000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 6) == 32000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 7) == 60000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, 20) == 60000);
    EXPECT(sp_backoff_compute_delay_ms(1000, 60000, UINT32_MAX) == 60000);
}

static void test_overflow_safe(void)
{
    /* Large initial near overflow path must still cap. */
    uint32_t d = sp_backoff_compute_delay_ms(1u << 30, 1u << 30, 40);
    EXPECT(d == (1u << 30));
}

static void test_jitter_bounds(void)
{
    sp_backoff_t state;
    sp_backoff_config_t cfg = {
        .initial_ms = 1000,
        .max_ms = 60000,
        .jitter_percent = 20,
        .stable_reset_ms = 0,
    };
    sp_backoff_init(&state, &cfg);
    sp_backoff_note_failure(&state, 0);
    uint32_t lo = sp_backoff_delay_ms(&state, fixed_rand);
    uint32_t hi = sp_backoff_delay_ms(&state, mid_rand);
    EXPECT(lo >= 800);  /* 1000 - 20% */
    EXPECT(lo <= 1200);
    EXPECT(hi >= 800);
    EXPECT(hi <= 1200);
}

static void test_reset_and_stable(void)
{
    sp_backoff_t mqtt;
    sp_backoff_config_t mqtt_cfg;
    sp_backoff_config_mqtt(&mqtt_cfg);
    mqtt_cfg.stable_reset_ms = 30000;
    sp_backoff_init(&mqtt, &mqtt_cfg);
    sp_backoff_note_failure(&mqtt, 0);
    sp_backoff_note_failure(&mqtt, 1000);
    EXPECT(mqtt.consecutive_failures == 2);
    EXPECT(mqtt.current_delay_ms == 2000);

    sp_backoff_note_success(&mqtt, 5000);
    EXPECT(mqtt.consecutive_failures == 2); /* not reset yet */
    EXPECT(sp_backoff_poll_stable(&mqtt, 20000) == false);
    EXPECT(sp_backoff_poll_stable(&mqtt, 36000) == true);
    EXPECT(mqtt.consecutive_failures == 0);
    EXPECT(mqtt.current_delay_ms == mqtt_cfg.initial_ms);

    sp_backoff_t https;
    sp_backoff_config_t https_cfg;
    sp_backoff_config_https(&https_cfg);
    sp_backoff_init(&https, &https_cfg);
    sp_backoff_note_failure(&https, 0);
    sp_backoff_note_failure(&https, 1);
    sp_backoff_note_success(&https, 2);
    EXPECT(https.consecutive_failures == 0);
}

static void test_independent_state(void)
{
    sp_backoff_t mqtt;
    sp_backoff_t https;
    sp_backoff_config_t mqtt_cfg;
    sp_backoff_config_t https_cfg;
    sp_backoff_config_mqtt(&mqtt_cfg);
    sp_backoff_config_https(&https_cfg);
    sp_backoff_init(&mqtt, &mqtt_cfg);
    sp_backoff_init(&https, &https_cfg);

    sp_backoff_note_failure(&mqtt, 0);
    sp_backoff_note_failure(&mqtt, 1);
    sp_backoff_note_failure(&mqtt, 2);
    EXPECT(mqtt.consecutive_failures == 3);
    EXPECT(https.consecutive_failures == 0);
    EXPECT(https.current_delay_ms == https_cfg.initial_ms);
}

static void test_in_flight_and_ready(void)
{
    sp_backoff_t state;
    sp_backoff_config_t cfg;
    sp_backoff_config_mqtt(&cfg);
    cfg.stable_reset_ms = 0;
    sp_backoff_init(&state, &cfg);

    EXPECT(sp_backoff_ready(&state, 0) == true);
    sp_backoff_set_in_flight(&state, true);
    EXPECT(sp_backoff_ready(&state, 0) == false);
    sp_backoff_set_in_flight(&state, false);
    sp_backoff_note_failure(&state, 1000);
    EXPECT(sp_backoff_ready(&state, 1500) == false);
    EXPECT(sp_backoff_ready(&state, 1000 + (int64_t)state.current_delay_ms) == true);
}

int main(void)
{
    test_growth_and_cap();
    test_overflow_safe();
    test_jitter_bounds();
    test_reset_and_stable();
    test_independent_state();
    test_in_flight_and_ready();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
