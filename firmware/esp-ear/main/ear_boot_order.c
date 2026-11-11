#include "ear_boot_order.h"

#include <stddef.h>

bool dbmap_ear_boot_allow_mic_start(const dbmap_ear_boot_state_t *s)
{
    if (s == NULL) {
        return false;
    }
    return !s->mic_start_attempted;
}

bool dbmap_ear_boot_allow_obs_start(const dbmap_ear_boot_state_t *s)
{
    if (s == NULL) {
        return false;
    }
    return s->desired_boot_done && !s->obs_start_attempted;
}
