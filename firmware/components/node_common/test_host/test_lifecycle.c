/**
 * Host unit tests for OTA reboot-pending lifecycle gate.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/components/node_common/include \
 *     firmware/components/node_common/lifecycle.c \
 *     firmware/components/node_common/test_host/test_lifecycle.c \
 *     -o /tmp/test_dbmap_lifecycle && /tmp/test_dbmap_lifecycle
 */
#include "dbmap_lifecycle.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_default_not_pending(void)
{
    dbmap_lifecycle_reset_for_test();
    EXPECT(dbmap_lifecycle_ota_reboot_pending() == false);
}

static void test_enter_pending_is_sticky(void)
{
    dbmap_lifecycle_reset_for_test();
    dbmap_lifecycle_enter_ota_reboot_pending();
    EXPECT(dbmap_lifecycle_ota_reboot_pending() == true);
    /* Re-entering remains pending; no clear except reboot / test reset. */
    dbmap_lifecycle_enter_ota_reboot_pending();
    EXPECT(dbmap_lifecycle_ota_reboot_pending() == true);
}

static void test_reconnect_gate_semantics(void)
{
    dbmap_lifecycle_reset_for_test();
    /* Before pending: reconnects are allowed (gate open). */
    EXPECT(dbmap_lifecycle_ota_reboot_pending() == false);
    dbmap_lifecycle_enter_ota_reboot_pending();
    /* After pending: callers must not start new reconnect attempts. */
    EXPECT(dbmap_lifecycle_ota_reboot_pending() == true);
}

int main(void)
{
    test_default_not_pending();
    test_enter_pending_is_sticky();
    test_reconnect_gate_semantics();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
