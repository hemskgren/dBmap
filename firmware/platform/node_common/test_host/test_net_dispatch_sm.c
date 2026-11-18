/**
 * Host tests for recovery dispatch. request() must not run the operation.
 *
 *   cc -std=c11 -Wall -Wextra -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/net_dispatch_sm.c \
 *     firmware/platform/node_common/test_host/test_net_dispatch_sm.c \
 *     -o /tmp/test_net_dispatch_sm && /tmp/test_net_dispatch_sm
 */
#include "sp_net_dispatch.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    int calls;
    sp_net_op_t last;
} exec_log_t;

static void exec_op(sp_net_op_t op, void *ctx)
{
    exec_log_t *log = ctx;
    log->calls++;
    log->last = op;
}

static void test_request_does_not_run_the_operation(void)
{
    sp_net_dispatch_sm_t sm;
    exec_log_t log = {0, SP_NET_OP_WIFI_CONNECT};
    sp_net_dispatch_sm_init(&sm);

    sp_net_dispatch_sm_request(&sm, SP_NET_OP_MQTT_RECONNECT);
    sp_net_dispatch_sm_request(&sm, SP_NET_OP_WIFI_CONNECT);
    EXPECT(log.calls == 0);
    EXPECT(sm.take_count == 0);
    EXPECT(sm.request_count == 2);

    /* Same op twice is one bit. The timer callback still has not run it. */
    sp_net_dispatch_sm_request(&sm, SP_NET_OP_WIFI_CONNECT);
    EXPECT(log.calls == 0);
    EXPECT(sm.request_count == 3);

    sp_net_op_t first = SP_NET_OP_COUNT;
    EXPECT(sp_net_dispatch_sm_take(&sm, &first) == true);
    EXPECT(first == SP_NET_OP_WIFI_CONNECT);
    EXPECT(log.calls == 0);

    exec_op(first, &log);
    EXPECT(log.calls == 1);
    EXPECT(log.last == SP_NET_OP_WIFI_CONNECT);
}

static void test_drain_is_the_only_executor(void)
{
    sp_net_dispatch_sm_t sm;
    exec_log_t log = {0, SP_NET_OP_WIFI_CONNECT};
    sp_net_dispatch_sm_init(&sm);
    sp_net_dispatch_sm_request(&sm, SP_NET_OP_MQTT_PROGRESS);
    sp_net_dispatch_sm_request(&sm, SP_NET_OP_WIFI_DISCONNECT_FOR_RETRY);
    EXPECT(log.calls == 0);

    sp_net_dispatch_sm_drain(&sm, exec_op, &log);
    EXPECT(log.calls == 2);
    EXPECT(log.last == SP_NET_OP_MQTT_PROGRESS);
    EXPECT(sm.pending == 0);
    EXPECT(sp_net_dispatch_sm_take(&sm, &log.last) == false);
}

int main(void)
{
    test_request_does_not_run_the_operation();
    test_drain_is_the_only_executor();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
