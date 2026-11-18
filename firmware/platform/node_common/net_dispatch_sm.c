#include "sp_net_dispatch.h"

#include <stddef.h>

void sp_net_dispatch_sm_init(sp_net_dispatch_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    sm->pending = 0;
    sm->request_count = 0;
    sm->take_count = 0;
}

void sp_net_dispatch_sm_request(sp_net_dispatch_sm_t *sm, sp_net_op_t op)
{
    if (sm == NULL || op >= SP_NET_OP_COUNT) {
        return;
    }
    sm->pending |= (1u << (uint32_t)op);
    if (sm->request_count < UINT32_MAX) {
        sm->request_count++;
    }
}

bool sp_net_dispatch_sm_take(sp_net_dispatch_sm_t *sm, sp_net_op_t *out)
{
    if (sm == NULL || out == NULL || sm->pending == 0) {
        return false;
    }
    for (uint32_t i = 0; i < (uint32_t)SP_NET_OP_COUNT; ++i) {
        uint32_t bit = 1u << i;
        if ((sm->pending & bit) == 0) {
            continue;
        }
        sm->pending &= ~bit;
        if (sm->take_count < UINT32_MAX) {
            sm->take_count++;
        }
        *out = (sp_net_op_t)i;
        return true;
    }
    return false;
}

void sp_net_dispatch_sm_drain(
    sp_net_dispatch_sm_t *sm,
    sp_net_dispatch_exec_fn exec,
    void *ctx
)
{
    sp_net_op_t op;
    if (sm == NULL || exec == NULL) {
        return;
    }
    while (sp_net_dispatch_sm_take(sm, &op)) {
        exec(op, ctx);
    }
}
