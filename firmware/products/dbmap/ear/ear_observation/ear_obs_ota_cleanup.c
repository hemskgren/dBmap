#include "ear_obs_ota_cleanup.h"

#include <stddef.h>

void ear_obs_ota_release_product_bindings(
    ear_obs_manager_t *om_or_null,
    ear_critical_path_t *critical_or_null,
    int64_t now_mono_us
)
{
    if (om_or_null != NULL) {
        ear_obs_manager_forget_bindings(om_or_null);
    }
    if (critical_or_null != NULL) {
        ear_critical_close_for_ota(critical_or_null, now_mono_us);
    }
}
